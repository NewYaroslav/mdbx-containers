#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_TRANSPORT_MIDDLEWARE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_TRANSPORT_MIDDLEWARE_HPP_INCLUDED

/// \file TransportMiddleware.hpp
/// \brief Policy and metrics wrappers for sync transport adapters.
/// \details
/// These helpers sit around \c ISyncPeer or \c IHttpSyncClient. They do not
/// add credentials to sync DTOs and do not own sockets; concrete transports can
/// use them to enforce allow lists, fixed request budgets, and metrics hooks.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "HttpTransport.hpp"
#include "ISyncPeer.hpp"
#include "WebSocketTransport.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Transport operation observed by middleware.
    enum class SyncTransportOperation : std::uint8_t {
        Pull,
        Push,
        HttpPost,
        WebSocketMessage
    };

    /// \brief Retry classification for adapter-level HTTP failures.
    enum class HttpSyncRetryClass : std::uint8_t {
        Success,
        Retryable,
        Permanent
    };

    /// \brief Classifies HTTP status codes for sync transport retry policy.
    /// \details This helper classifies transport-level response status only.
    /// Sync DTO responses with HTTP 200 still carry their own \c ok/error
    /// fields and must be handled by the caller.
    inline HttpSyncRetryClass classify_http_sync_status(unsigned status_code) {
        if (status_code >= 200 && status_code < 300) {
            return HttpSyncRetryClass::Success;
        }
        switch (status_code) {
            case 408:
            case 425:
            case 429:
            case 500:
            case 502:
            case 503:
            case 504:
                return HttpSyncRetryClass::Retryable;
            default:
                return HttpSyncRetryClass::Permanent;
        }
    }

    /// \brief Returns true when an HTTP sync status is retryable.
    inline bool http_sync_status_is_retryable(unsigned status_code) {
        return classify_http_sync_status(status_code) ==
               HttpSyncRetryClass::Retryable;
    }

    /// \brief Result returned by a transport policy.
    struct SyncTransportDecision {
        bool allowed = true;
        unsigned status_code = 0;
        std::string error;
        std::vector<HttpSyncHeader> response_headers;

        static SyncTransportDecision allow() {
            return SyncTransportDecision();
        }

        static SyncTransportDecision reject(const std::string& message,
                                            unsigned status = 403) {
            SyncTransportDecision out;
            out.allowed = false;
            out.status_code = status;
            out.error = message;
            return out;
        }

        void add_response_header(const std::string& name,
                                 const std::string& value) {
            http_add_header(response_headers, name, value);
        }
    };

    /// \brief Adapter-local context for one WebSocket sync message.
    /// \details Concrete WebSocket bindings authenticate the session during
    /// handshake or with framework-specific metadata, then pass the resulting
    /// node identity here before dispatching the binary sync message. The
    /// identity and DB allow-list are not serialized in the sync DTO.
    struct WebSocketSyncRequestContext {
        bool has_authenticated_node = false;
        NodeId authenticated_node{};
        std::set<DbId> allowed_dbs;
        std::vector<std::uint8_t> binary_message;
    };

    /// \brief Policy hook for sync transport requests.
    /// \details Default methods allow every request. Implementations may
    /// inspect DTO metadata, route names, or message sizes before forwarding.
    class ISyncTransportPolicy {
    public:
        virtual ~ISyncTransportPolicy() {}

        virtual SyncTransportDecision check_pull(
                const PullRequest& request) {
            (void)request;
            return SyncTransportDecision::allow();
        }

        virtual SyncTransportDecision check_push(
                const PushRequest& request) {
            (void)request;
            return SyncTransportDecision::allow();
        }

        virtual SyncTransportDecision check_http_post(
                const std::string& target,
                const std::string& content_type,
                const std::vector<std::uint8_t>& body) {
            (void)target;
            (void)content_type;
            (void)body;
            return SyncTransportDecision::allow();
        }

        virtual SyncTransportDecision check_http_request(
                const HttpSyncRequest& request) {
            return check_http_post(request.target,
                                   request.content_type,
                                   request.body);
        }

        virtual SyncTransportDecision check_websocket_message(
                const WebSocketSyncRequestContext& request) {
            (void)request;
            return SyncTransportDecision::allow();
        }
    };

    /// \brief Extracts a bearer token from an HTTP sync request.
    inline std::string http_bearer_token(const HttpSyncRequest& request) {
        static const char scheme[] = "bearer";
        const std::string value =
            http_header_value(request.headers, "Authorization");
        if (value.size() <= sizeof(scheme)) {
            return std::string();
        }
        for (std::size_t i = 0; i < sizeof(scheme) - 1u; ++i) {
            if (http_ascii_lower(value[i]) != scheme[i]) {
                return std::string();
            }
        }
        if (value[sizeof(scheme) - 1u] != ' ') {
            return std::string();
        }
        return value.substr(sizeof(scheme));
    }

    /// \brief Allows only configured node ids and database ids.
    /// \details Empty allow-list means "allow any" for that dimension.
    class NodeDbAllowListPolicy : public ISyncTransportPolicy {
    public:
        void allow_node_id(const NodeId& node_id) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allowed_nodes.insert(node_id);
        }

        void allow_db_id(const DbId& db_id) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allowed_dbs.insert(db_id);
        }

        void clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allowed_nodes.clear();
            m_allowed_dbs.clear();
        }

        SyncTransportDecision check_pull(
                const PullRequest& request) override {
            return check_node_and_db(request.requester,
                                     request.db_id,
                                     "sync requester is not allowed");
        }

        SyncTransportDecision check_push(
                const PushRequest& request) override {
            return check_node_and_db(request.sender,
                                     request.db_id,
                                     "sync sender is not allowed");
        }

    private:
        SyncTransportDecision check_node_and_db(
                const NodeId& node_id,
                const DbId& db_id,
                const char* node_error) const {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_allowed_dbs.empty() &&
                m_allowed_dbs.find(db_id) == m_allowed_dbs.end()) {
                return SyncTransportDecision::reject(
                    "sync db_id is not allowed", 403);
            }
            if (!m_allowed_nodes.empty() &&
                m_allowed_nodes.find(node_id) == m_allowed_nodes.end()) {
                return SyncTransportDecision::reject(node_error, 403);
            }
            return SyncTransportDecision::allow();
        }

        mutable std::mutex m_mutex;
        std::set<NodeId> m_allowed_nodes;
        std::set<DbId> m_allowed_dbs;
    };

    /// \brief Allows only configured HTTP sync targets.
    /// \details Empty allow-list means every target is allowed.
    class HttpRouteAllowListPolicy : public ISyncTransportPolicy {
    public:
        void allow_target(const std::string& target) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allowed_targets.insert(target);
        }

        void clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allowed_targets.clear();
        }

        SyncTransportDecision check_http_post(
                const std::string& target,
                const std::string& content_type,
                const std::vector<std::uint8_t>& body) override {
            (void)content_type;
            (void)body;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_allowed_targets.empty() &&
                m_allowed_targets.find(target) == m_allowed_targets.end()) {
                return SyncTransportDecision::reject(
                    "sync HTTP route is not allowed", 403);
            }
            return SyncTransportDecision::allow();
        }

    private:
        mutable std::mutex m_mutex;
        std::set<std::string> m_allowed_targets;
    };

    /// \brief Allows only configured bearer tokens on HTTP sync requests.
    /// \details Empty allow-list means every token is allowed, but a token must
    /// still be present.
    class HttpBearerTokenPolicy : public ISyncTransportPolicy {
    public:
        void allow_token(const std::string& token) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allowed_tokens.insert(token);
        }

        void clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allowed_tokens.clear();
        }

        SyncTransportDecision check_http_request(
                const HttpSyncRequest& request) override {
            const std::string token = http_bearer_token(request);
            if (token.empty()) {
                SyncTransportDecision decision =
                    SyncTransportDecision::reject(
                        "sync bearer token is missing", 401);
                decision.add_response_header(
                    "WWW-Authenticate",
                    "Bearer realm=\"mdbx-containers-sync\"");
                return decision;
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_allowed_tokens.empty() &&
                m_allowed_tokens.find(token) == m_allowed_tokens.end()) {
                SyncTransportDecision decision =
                    SyncTransportDecision::reject(
                        "sync bearer token is not allowed", 401);
                decision.add_response_header(
                    "WWW-Authenticate",
                    "Bearer realm=\"mdbx-containers-sync\"");
                return decision;
            }
            return SyncTransportDecision::allow();
        }

    private:
        mutable std::mutex m_mutex;
        std::set<std::string> m_allowed_tokens;
    };

    /// \brief Binds HTTP bearer tokens to sync \c NodeId values.
    /// \details This policy enforces the production-facing identity contract:
    /// the authenticated bearer principal must match
    /// \c PullRequest::requester for pull and \c PushRequest::sender for push.
    /// Optional per-token DB allow-lists validate \c db_id before dispatch.
    /// Empty per-token DB allow-list means any \c db_id is allowed for that
    /// token. The policy decodes request DTOs only after the bearer token is
    /// accepted; adapter-local headers and remote addresses are not serialized.
    class HttpBearerNodeIdentityPolicy : public ISyncTransportPolicy {
    public:
        explicit HttpBearerNodeIdentityPolicy(
                const CodecBounds& bounds = CodecBounds())
            : m_bounds(bounds) {}

        void allow_token_for_node(const std::string& token,
                                  const NodeId& node_id) {
            std::lock_guard<std::mutex> lock(m_mutex);
            Binding& binding = m_bindings[token];
            binding.node_id = node_id;
        }

        void allow_db_id_for_token(const std::string& token,
                                   const DbId& db_id) {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::map<std::string, Binding>::iterator it =
                m_bindings.find(token);
            if (it == m_bindings.end()) {
                throw std::invalid_argument(
                    "sync bearer token identity is not registered");
            }
            it->second.allowed_dbs.insert(db_id);
        }

        void clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_bindings.clear();
        }

        SyncTransportDecision check_http_request(
                const HttpSyncRequest& request) override {
            const std::string token = http_bearer_token(request);
            if (token.empty()) {
                return unauthorized("sync bearer token is missing");
            }

            Binding binding;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                std::map<std::string, Binding>::const_iterator it =
                    m_bindings.find(token);
                if (it == m_bindings.end()) {
                    return unauthorized(
                        "sync bearer token identity is not allowed");
                }
                binding = it->second;
            }

            if (request.content_type != HttpSyncRoutes::content_type()) {
                return SyncTransportDecision::allow();
            }
            if (request.target == HttpSyncRoutes::pull_target()) {
                return check_pull_identity(request.body, binding);
            }
            if (request.target == HttpSyncRoutes::push_target()) {
                return check_push_identity(request.body, binding);
            }
            return SyncTransportDecision::allow();
        }

    private:
        struct Binding {
            NodeId node_id{};
            std::set<DbId> allowed_dbs;
        };

        static SyncTransportDecision unauthorized(
                const std::string& message) {
            SyncTransportDecision decision =
                SyncTransportDecision::reject(message, 401);
            decision.add_response_header(
                "WWW-Authenticate",
                "Bearer realm=\"mdbx-containers-sync\"");
            return decision;
        }

        SyncTransportDecision check_pull_identity(
                const std::vector<std::uint8_t>& body,
                const Binding& binding) const {
            try {
                const PullRequest request =
                    TransportMessageCodec::decode_pull_request(
                        body, &m_bounds);
                if (request.requester != binding.node_id) {
                    return SyncTransportDecision::reject(
                        "sync requester does not match authenticated node",
                        403);
                }
                return check_db(binding, request.db_id);
            } catch (const std::length_error& e) {
                return SyncTransportDecision::reject(e.what(), 413);
            } catch (const std::exception& e) {
                return SyncTransportDecision::reject(e.what(), 400);
            }
        }

        SyncTransportDecision check_push_identity(
                const std::vector<std::uint8_t>& body,
                const Binding& binding) const {
            try {
                const PushRequest request =
                    TransportMessageCodec::decode_push_request(
                        body, &m_bounds);
                if (request.sender != binding.node_id) {
                    return SyncTransportDecision::reject(
                        "sync sender does not match authenticated node",
                        403);
                }
                return check_db(binding, request.db_id);
            } catch (const std::length_error& e) {
                return SyncTransportDecision::reject(e.what(), 413);
            } catch (const std::exception& e) {
                return SyncTransportDecision::reject(e.what(), 400);
            }
        }

        static SyncTransportDecision check_db(const Binding& binding,
                                              const DbId& db_id) {
            if (!binding.allowed_dbs.empty() &&
                binding.allowed_dbs.find(db_id) ==
                    binding.allowed_dbs.end()) {
                return SyncTransportDecision::reject(
                    "sync db_id is not allowed for authenticated node", 403);
            }
            return SyncTransportDecision::allow();
        }

        mutable std::mutex m_mutex;
        CodecBounds m_bounds;
        std::map<std::string, Binding> m_bindings;
    };

    /// \brief Binds a WebSocket session identity to sync \c NodeId values.
    /// \details Concrete WebSocket bindings should authenticate the session
    /// before constructing \c WebSocketSyncRequestContext. This policy then
    /// decodes the binary DTO and requires \c PullRequest::requester or
    /// \c PushRequest::sender to match the authenticated session node.
    /// Optional per-session DB allow-lists validate \c db_id before dispatch.
    /// Empty \c allowed_dbs means the authenticated session may access any
    /// \c db_id. For WebSocket bindings, \c SyncTransportDecision::status_code
    /// may be interpreted as a WebSocket close code by the concrete adapter.
    class WebSocketAuthenticatedNodeIdentityPolicy
        : public ISyncTransportPolicy {
    public:
        explicit WebSocketAuthenticatedNodeIdentityPolicy(
                const CodecBounds& bounds = CodecBounds())
            : m_bounds(bounds) {}

        SyncTransportDecision check_websocket_message(
                const WebSocketSyncRequestContext& request) override {
            if (!request.has_authenticated_node) {
                return SyncTransportDecision::reject(
                    "sync WebSocket authenticated node is missing", 1008);
            }

            try {
                const TransportMessageType type =
                    TransportMessageCodec::peek_message_type(
                        request.binary_message, &m_bounds);
                switch (type) {
                    case TransportMessageType::PullRequest:
                        return check_pull_identity(request);
                    case TransportMessageType::PushRequest:
                        return check_push_identity(request);
                    case TransportMessageType::PullResponse:
                    case TransportMessageType::PushResponse:
                        return SyncTransportDecision::reject(
                            "sync WebSocket server received response message",
                            1008);
                }
            } catch (const std::length_error& e) {
                return SyncTransportDecision::reject(e.what(), 1009);
            } catch (const std::exception& e) {
                return SyncTransportDecision::reject(e.what(), 1007);
            }
            return SyncTransportDecision::reject(
                "Unexpected WebSocket sync message type", 1008);
        }

    private:
        SyncTransportDecision check_pull_identity(
                const WebSocketSyncRequestContext& context) const {
            const PullRequest request =
                TransportMessageCodec::decode_pull_request(
                    context.binary_message, &m_bounds);
            if (request.requester != context.authenticated_node) {
                return SyncTransportDecision::reject(
                    "sync requester does not match authenticated WebSocket node",
                    1008);
            }
            return check_db(context.allowed_dbs, request.db_id);
        }

        SyncTransportDecision check_push_identity(
                const WebSocketSyncRequestContext& context) const {
            const PushRequest request =
                TransportMessageCodec::decode_push_request(
                    context.binary_message, &m_bounds);
            if (request.sender != context.authenticated_node) {
                return SyncTransportDecision::reject(
                    "sync sender does not match authenticated WebSocket node",
                    1008);
            }
            return check_db(context.allowed_dbs, request.db_id);
        }

        static SyncTransportDecision check_db(
                const std::set<DbId>& allowed_dbs,
                const DbId& db_id) {
            if (!allowed_dbs.empty() &&
                allowed_dbs.find(db_id) == allowed_dbs.end()) {
                return SyncTransportDecision::reject(
                    "sync db_id is not allowed for authenticated WebSocket node",
                    1008);
            }
            return SyncTransportDecision::allow();
        }

        CodecBounds m_bounds;
    };

    /// \brief Allows only configured remote addresses on HTTP sync requests.
    /// \details Empty allow-list means every remote address is allowed.
    class HttpRemoteAddressAllowListPolicy : public ISyncTransportPolicy {
    public:
        void allow_remote_address(const std::string& remote_address) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allowed_remote_addresses.insert(remote_address);
        }

        void clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allowed_remote_addresses.clear();
        }

        SyncTransportDecision check_http_request(
                const HttpSyncRequest& request) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_allowed_remote_addresses.empty() &&
                m_allowed_remote_addresses.find(request.remote_address) ==
                    m_allowed_remote_addresses.end()) {
                return SyncTransportDecision::reject(
                    "sync remote address is not allowed", 403);
            }
            return SyncTransportDecision::allow();
        }

    private:
        mutable std::mutex m_mutex;
        std::set<std::string> m_allowed_remote_addresses;
    };

    /// \brief Fixed-window HTTP request limiter keyed by token or remote address.
    /// \details Uses the bearer token when present, otherwise \c remote_address,
    /// otherwise the literal "anonymous". Rejections include a \c Retry-After
    /// header with the remaining window time in seconds.
    class FixedWindowHttpRateLimitPolicy : public ISyncTransportPolicy {
    public:
        FixedWindowHttpRateLimitPolicy(std::uint64_t max_requests,
                                       std::chrono::seconds window)
            : m_max_requests(max_requests), m_window(window) {
            if (window.count() <= 0) {
                throw std::invalid_argument(
                    "HTTP rate-limit window must be positive");
            }
        }

        void clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_buckets.clear();
        }

        SyncTransportDecision check_http_request(
                const HttpSyncRequest& request) override {
            const std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now();
            const std::string identity = client_identity(request);

            std::lock_guard<std::mutex> lock(m_mutex);
            Bucket& bucket = m_buckets[identity];
            if (bucket.reset_at == std::chrono::steady_clock::time_point() ||
                now >= bucket.reset_at) {
                bucket.remaining = m_max_requests;
                bucket.reset_at = now + m_window;
            }

            if (bucket.remaining == 0) {
                SyncTransportDecision decision =
                    SyncTransportDecision::reject(
                        "sync HTTP rate limit exceeded", 429);
                decision.add_response_header(
                    "Retry-After",
                    retry_after_seconds(now, bucket.reset_at));
                return decision;
            }
            --bucket.remaining;
            return SyncTransportDecision::allow();
        }

    private:
        struct Bucket {
            Bucket() : remaining(0), reset_at() {}

            std::uint64_t remaining;
            std::chrono::steady_clock::time_point reset_at;
        };

        static std::string client_identity(const HttpSyncRequest& request) {
            const std::string token = http_bearer_token(request);
            if (!token.empty()) {
                return std::string("token:") + token;
            }
            if (!request.remote_address.empty()) {
                return std::string("remote:") + request.remote_address;
            }
            return "anonymous";
        }

        static std::string retry_after_seconds(
                std::chrono::steady_clock::time_point now,
                std::chrono::steady_clock::time_point reset_at) {
            if (reset_at <= now) {
                return "0";
            }
            const std::chrono::seconds seconds =
                std::chrono::duration_cast<std::chrono::seconds>(
                    reset_at - now);
            const std::uint64_t count =
                seconds.count() <= 0 ? 1u
                                     : static_cast<std::uint64_t>(
                                           seconds.count());
            return std::to_string(count);
        }

        mutable std::mutex m_mutex;
        std::uint64_t m_max_requests;
        std::chrono::seconds m_window;
        std::map<std::string, Bucket> m_buckets;
    };

    /// \brief Fixed request-budget policy for deterministic rate-limit tests.
    /// \details Each accepted operation consumes one budget unit. Use
    /// \c unlimited_budget() for dimensions that should not be limited.
    class FixedBudgetSyncTransportPolicy : public ISyncTransportPolicy {
    public:
        static std::uint64_t unlimited_budget() {
            return std::numeric_limits<std::uint64_t>::max();
        }

        FixedBudgetSyncTransportPolicy(
                std::uint64_t pull_budget,
                std::uint64_t push_budget,
                std::uint64_t http_post_budget = unlimited_budget())
            : m_pull_remaining(pull_budget),
              m_push_remaining(push_budget),
              m_http_post_remaining(http_post_budget) {}

        void reset(std::uint64_t pull_budget,
                   std::uint64_t push_budget,
                   std::uint64_t http_post_budget = unlimited_budget()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pull_remaining = pull_budget;
            m_push_remaining = push_budget;
            m_http_post_remaining = http_post_budget;
        }

        SyncTransportDecision check_pull(
                const PullRequest& request) override {
            (void)request;
            return consume(m_pull_remaining, "sync pull rate limit exceeded");
        }

        SyncTransportDecision check_push(
                const PushRequest& request) override {
            (void)request;
            return consume(m_push_remaining, "sync push rate limit exceeded");
        }

        SyncTransportDecision check_http_post(
                const std::string& target,
                const std::string& content_type,
                const std::vector<std::uint8_t>& body) override {
            (void)target;
            (void)content_type;
            (void)body;
            return consume(m_http_post_remaining,
                           "sync HTTP post rate limit exceeded");
        }

    private:
        SyncTransportDecision consume(std::uint64_t& remaining,
                                      const char* error) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (remaining == unlimited_budget()) {
                return SyncTransportDecision::allow();
            }
            if (remaining == 0) {
                return SyncTransportDecision::reject(error, 429);
            }
            --remaining;
            return SyncTransportDecision::allow();
        }

        mutable std::mutex m_mutex;
        std::uint64_t m_pull_remaining;
        std::uint64_t m_push_remaining;
        std::uint64_t m_http_post_remaining;
    };

    /// \brief Runs several policies in insertion order.
    /// \details Added policies are non-owning references and must outlive the
    /// composite. Build the policy list before publishing the composite to
    /// worker threads; calling \c add() concurrently with \c check_*() is not
    /// supported.
    class CompositeSyncTransportPolicy : public ISyncTransportPolicy {
    public:
        void add(ISyncTransportPolicy& policy) {
            m_policies.push_back(&policy);
        }

        SyncTransportDecision check_pull(
                const PullRequest& request) override {
            for (std::size_t i = 0; i < m_policies.size(); ++i) {
                const SyncTransportDecision decision =
                    m_policies[i]->check_pull(request);
                if (!decision.allowed) {
                    return decision;
                }
            }
            return SyncTransportDecision::allow();
        }

        SyncTransportDecision check_push(
                const PushRequest& request) override {
            for (std::size_t i = 0; i < m_policies.size(); ++i) {
                const SyncTransportDecision decision =
                    m_policies[i]->check_push(request);
                if (!decision.allowed) {
                    return decision;
                }
            }
            return SyncTransportDecision::allow();
        }

        SyncTransportDecision check_http_post(
                const std::string& target,
                const std::string& content_type,
                const std::vector<std::uint8_t>& body) override {
            for (std::size_t i = 0; i < m_policies.size(); ++i) {
                const SyncTransportDecision decision =
                    m_policies[i]->check_http_post(target, content_type, body);
                if (!decision.allowed) {
                    return decision;
                }
            }
            return SyncTransportDecision::allow();
        }

        SyncTransportDecision check_http_request(
                const HttpSyncRequest& request) override {
            for (std::size_t i = 0; i < m_policies.size(); ++i) {
                const SyncTransportDecision decision =
                    m_policies[i]->check_http_request(request);
                if (!decision.allowed) {
                    return decision;
                }
            }
            return SyncTransportDecision::allow();
        }

        SyncTransportDecision check_websocket_message(
                const WebSocketSyncRequestContext& request) override {
            for (std::size_t i = 0; i < m_policies.size(); ++i) {
                const SyncTransportDecision decision =
                    m_policies[i]->check_websocket_message(request);
                if (!decision.allowed) {
                    return decision;
                }
            }
            return SyncTransportDecision::allow();
        }

    private:
        std::vector<ISyncTransportPolicy*> m_policies;
    };

    /// \brief Snapshot of counters recorded by \c SyncTransportMetricsObserver.
    /// \details Counters represent middleware hook invocations. If the same
    /// observer is installed at several stacked middleware layers, one logical
    /// user action may increment more than one counter.
    struct SyncTransportMetricsSnapshot {
        std::uint64_t pull_calls = 0;
        std::uint64_t push_calls = 0;
        std::uint64_t http_post_calls = 0;
        std::uint64_t rejected_calls = 0;
        std::uint64_t failed_calls = 0;
        std::uint64_t request_cancel_calls = 0;
        std::uint64_t pulled_batches = 0;
        std::uint64_t pushed_batches = 0;
    };

    /// \brief Best-effort observer for transport middleware events.
    /// \details Middleware wrappers swallow observer exceptions so logging or
    /// metrics hooks cannot change transport behavior.
    class ISyncTransportObserver {
    public:
        virtual ~ISyncTransportObserver() {}

        virtual void on_sync_transport_rejected(
                SyncTransportOperation operation,
                const std::string& error) {
            (void)operation;
            (void)error;
        }

        virtual void on_sync_transport_pull_result(
                const PullRequest& request,
                const PullResponse& response) {
            (void)request;
            (void)response;
        }

        virtual void on_sync_transport_push_result(
                const PushRequest& request,
                const PushResponse& response) {
            (void)request;
            (void)response;
        }

        virtual void on_sync_transport_http_post_result(
                const std::string& target,
                const HttpSyncResponse& response) {
            (void)target;
            (void)response;
        }

        virtual void on_sync_transport_exception(
                SyncTransportOperation operation,
                const std::string& error) {
            (void)operation;
            (void)error;
        }

        virtual void on_sync_transport_cancel_requested() {}
    };

    /// \brief Thread-safe counter observer for basic transport metrics.
    /// \details Counts middleware hook invocations, not necessarily distinct
    /// logical user operations. Use separate observers when peer-level and
    /// HTTP-level layers should be reported independently.
    class SyncTransportMetricsObserver : public ISyncTransportObserver {
    public:
        SyncTransportMetricsSnapshot snapshot() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_snapshot;
        }

        void reset() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot = SyncTransportMetricsSnapshot();
        }

        void on_sync_transport_rejected(
                SyncTransportOperation operation,
                const std::string& error) override {
            (void)operation;
            (void)error;
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_snapshot.rejected_calls;
        }

        void on_sync_transport_pull_result(
                const PullRequest& request,
                const PullResponse& response) override {
            (void)request;
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_snapshot.pull_calls;
            if (!response.ok) {
                ++m_snapshot.failed_calls;
            }
            m_snapshot.pulled_batches += response.batches.size();
        }

        void on_sync_transport_push_result(
                const PushRequest& request,
                const PushResponse& response) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_snapshot.push_calls;
            if (!response.ok) {
                ++m_snapshot.failed_calls;
            }
            m_snapshot.pushed_batches += request.batches.size();
        }

        void on_sync_transport_http_post_result(
                const std::string& target,
                const HttpSyncResponse& response) override {
            (void)target;
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_snapshot.http_post_calls;
            if (response.status_code < 200 || response.status_code >= 300) {
                ++m_snapshot.failed_calls;
            }
        }

        void on_sync_transport_exception(
                SyncTransportOperation operation,
                const std::string& error) override {
            (void)operation;
            (void)error;
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_snapshot.failed_calls;
        }

        void on_sync_transport_cancel_requested() override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_snapshot.request_cancel_calls;
        }

    private:
        mutable std::mutex m_mutex;
        SyncTransportMetricsSnapshot m_snapshot;
    };

    namespace detail {

        inline void notify_transport_rejected(
                ISyncTransportObserver* observer,
                SyncTransportOperation operation,
                const std::string& error) {
            if (observer == nullptr) {
                return;
            }
            try {
                observer->on_sync_transport_rejected(operation, error);
            } catch (...) {}
        }

        inline void notify_transport_exception(
                ISyncTransportObserver* observer,
                SyncTransportOperation operation,
                const std::string& error) {
            if (observer == nullptr) {
                return;
            }
            try {
                observer->on_sync_transport_exception(operation, error);
            } catch (...) {}
        }

        inline void notify_transport_cancel_requested(
                ISyncTransportObserver* observer) {
            if (observer == nullptr) {
                return;
            }
            try {
                observer->on_sync_transport_cancel_requested();
            } catch (...) {}
        }

        inline HttpSyncResponse make_http_error_response(
                const SyncTransportDecision& decision,
                const char* fallback) {
            HttpSyncResponse response;
            response.status_code =
                decision.status_code == 0 ? 403 : decision.status_code;
            response.content_type = "text/plain; charset=utf-8";
            response.headers = decision.response_headers;
            response.error = decision.error.empty()
                ? std::string(fallback)
                : decision.error;
            response.body.assign(response.error.begin(), response.error.end());
            return response;
        }

    } // namespace detail

    /// \brief \c ISyncPeer wrapper that applies policy and observer hooks.
    /// \details The wrapped peer must outlive the middleware. The optional
    /// policy and observer are non-owning pointers and must also outlive the
    /// middleware when provided.
    class SyncPeerMiddleware : public ISyncPeer {
    public:
        explicit SyncPeerMiddleware(
                ISyncPeer& next,
                ISyncTransportPolicy* policy = nullptr,
                ISyncTransportObserver* observer = nullptr)
            : m_next(next),
              m_policy(policy),
              m_observer(observer) {}

        PullResponse pull(const PullRequest& request) override {
            if (m_policy != nullptr) {
                const SyncTransportDecision decision =
                    m_policy->check_pull(request);
                if (!decision.allowed) {
                    detail::notify_transport_rejected(
                        m_observer, SyncTransportOperation::Pull,
                        decision.error);
                    PullResponse response;
                    response.ok = false;
                    response.error = reject_message(decision, "pull rejected");
                    return response;
                }
            }

            try {
                const PullResponse response = m_next.pull(request);
                notify_pull_result(request, response);
                return response;
            } catch (const std::exception& e) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::Pull, e.what());
                throw;
            } catch (...) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::Pull,
                    "unknown pull exception");
                throw;
            }
        }

        PushResponse push(const PushRequest& request) override {
            if (m_policy != nullptr) {
                const SyncTransportDecision decision =
                    m_policy->check_push(request);
                if (!decision.allowed) {
                    detail::notify_transport_rejected(
                        m_observer, SyncTransportOperation::Push,
                        decision.error);
                    PushResponse response;
                    response.ok = false;
                    response.error = reject_message(decision, "push rejected");
                    return response;
                }
            }

            try {
                const PushResponse response = m_next.push(request);
                notify_push_result(request, response);
                return response;
            } catch (const std::exception& e) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::Push, e.what());
                throw;
            } catch (...) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::Push,
                    "unknown push exception");
                throw;
            }
        }

        void request_cancel() override {
            detail::notify_transport_cancel_requested(m_observer);
            m_next.request_cancel();
        }

    private:
        static std::string reject_message(
                const SyncTransportDecision& decision,
                const char* fallback) {
            return decision.error.empty() ? std::string(fallback)
                                          : decision.error;
        }

        void notify_pull_result(const PullRequest& request,
                                const PullResponse& response) const {
            if (m_observer == nullptr) {
                return;
            }
            try {
                m_observer->on_sync_transport_pull_result(request, response);
            } catch (...) {}
        }

        void notify_push_result(const PushRequest& request,
                                const PushResponse& response) const {
            if (m_observer == nullptr) {
                return;
            }
            try {
                m_observer->on_sync_transport_push_result(request, response);
            } catch (...) {}
        }

        ISyncPeer& m_next;
        ISyncTransportPolicy* m_policy;
        ISyncTransportObserver* m_observer;
    };

    /// \brief \c HttpSyncServer wrapper that applies request-context policy.
    /// \details The wrapped server must outlive the middleware. The optional
    /// policy and observer are non-owning pointers and must also outlive the
    /// middleware when provided.
    class HttpSyncServerMiddleware {
    public:
        explicit HttpSyncServerMiddleware(
                HttpSyncServer& next,
                ISyncTransportPolicy* policy = nullptr,
                ISyncTransportObserver* observer = nullptr)
            : m_next(next),
              m_policy(policy),
              m_observer(observer) {}

        HttpSyncResponse handle(const HttpSyncRequest& request) {
            if (m_policy != nullptr) {
                const SyncTransportDecision decision =
                    m_policy->check_http_request(request);
                if (!decision.allowed) {
                    detail::notify_transport_rejected(
                        m_observer, SyncTransportOperation::HttpPost,
                        decision.error);
                    return detail::make_http_error_response(
                        decision, "HTTP sync request rejected");
                }
            }

            try {
                const HttpSyncResponse response = m_next.handle(request);
                notify_http_post_result(request.target, response);
                return response;
            } catch (const std::exception& e) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::HttpPost, e.what());
                throw;
            } catch (...) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::HttpPost,
                    "unknown HTTP server exception");
                throw;
            }
        }

    private:
        void notify_http_post_result(const std::string& target,
                                     const HttpSyncResponse& response) const {
            if (m_observer == nullptr) {
                return;
            }
            try {
                m_observer->on_sync_transport_http_post_result(target, response);
            } catch (...) {}
        }

        HttpSyncServer& m_next;
        ISyncTransportPolicy* m_policy;
        ISyncTransportObserver* m_observer;
    };

    /// \brief \c WebSocketSyncServer wrapper that applies session policy.
    /// \details The wrapped server must outlive the middleware. Concrete
    /// WebSocket bindings provide an adapter-local request context containing
    /// the authenticated session node and one complete binary sync message.
    class WebSocketSyncServerMiddleware {
    public:
        explicit WebSocketSyncServerMiddleware(
                WebSocketSyncServer& next,
                ISyncTransportPolicy* policy = nullptr,
                ISyncTransportObserver* observer = nullptr)
            : m_next(next),
              m_policy(policy),
              m_observer(observer) {}

        std::vector<std::uint8_t> handle_binary_message(
                const WebSocketSyncRequestContext& request) {
            if (m_policy != nullptr) {
                const SyncTransportDecision decision =
                    m_policy->check_websocket_message(request);
                if (!decision.allowed) {
                    detail::notify_transport_rejected(
                        m_observer, SyncTransportOperation::WebSocketMessage,
                        decision.error);
                    throw std::runtime_error(reject_message(
                        decision, "WebSocket sync request rejected"));
                }
            }

            try {
                return m_next.handle_binary_message(request.binary_message);
            } catch (const std::exception& e) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::WebSocketMessage,
                    e.what());
                throw;
            } catch (...) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::WebSocketMessage,
                    "unknown WebSocket server exception");
                throw;
            }
        }

    private:
        static std::string reject_message(
                const SyncTransportDecision& decision,
                const char* fallback) {
            return decision.error.empty() ? std::string(fallback)
                                          : decision.error;
        }

        WebSocketSyncServer& m_next;
        ISyncTransportPolicy* m_policy;
        ISyncTransportObserver* m_observer;
    };

    /// \brief \c IHttpSyncClient wrapper that applies route-level policy.
    /// \details The wrapped client must outlive the middleware. The optional
    /// policy and observer are non-owning pointers and must also outlive the
    /// middleware when provided.
    class HttpSyncClientMiddleware : public IHttpSyncClient {
    public:
        explicit HttpSyncClientMiddleware(
                IHttpSyncClient& next,
                ISyncTransportPolicy* policy = nullptr,
                ISyncTransportObserver* observer = nullptr)
            : m_next(next),
              m_policy(policy),
              m_observer(observer) {}

        HttpSyncResponse post(
                const std::string& target,
                const std::string& content_type,
                const std::vector<std::uint8_t>& body,
                const CancellationToken& cancel_token) override {
            HttpSyncRequest request;
            request.method = HttpSyncRoutes::method_post();
            request.target = target;
            request.content_type = content_type;
            request.body = body;

            if (m_policy != nullptr) {
                const SyncTransportDecision decision =
                    m_policy->check_http_request(request);
                if (!decision.allowed) {
                    detail::notify_transport_rejected(
                        m_observer, SyncTransportOperation::HttpPost,
                        decision.error);
                    return detail::make_http_error_response(
                        decision, "HTTP sync request rejected");
                }
            }

            try {
                const HttpSyncResponse response =
                    m_next.post(target, content_type, body, cancel_token);
                notify_http_post_result(target, response);
                return response;
            } catch (const std::exception& e) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::HttpPost, e.what());
                throw;
            } catch (...) {
                detail::notify_transport_exception(
                    m_observer, SyncTransportOperation::HttpPost,
                    "unknown HTTP post exception");
                throw;
            }
        }

        void request_cancel() override {
            detail::notify_transport_cancel_requested(m_observer);
            m_next.request_cancel();
        }

    private:
        void notify_http_post_result(const std::string& target,
                                     const HttpSyncResponse& response) const {
            if (m_observer == nullptr) {
                return;
            }
            try {
                m_observer->on_sync_transport_http_post_result(target, response);
            } catch (...) {}
        }

        IHttpSyncClient& m_next;
        ISyncTransportPolicy* m_policy;
        ISyncTransportObserver* m_observer;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_TRANSPORT_MIDDLEWARE_HPP_INCLUDED
