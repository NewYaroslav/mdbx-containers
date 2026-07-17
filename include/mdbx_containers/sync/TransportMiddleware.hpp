#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_TRANSPORT_MIDDLEWARE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_TRANSPORT_MIDDLEWARE_HPP_INCLUDED

/// \file TransportMiddleware.hpp
/// \brief Policy and metrics wrappers for sync transport adapters.
/// \details
/// These helpers sit around \c ISyncPeer or \c IHttpSyncClient. They do not
/// add credentials to sync DTOs and do not own sockets; concrete transports can
/// use them to enforce allow lists, fixed request budgets, and metrics hooks.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "HttpTransport.hpp"
#include "ISyncPeer.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Transport operation observed by middleware.
    enum class SyncTransportOperation : std::uint8_t {
        Pull,
        Push,
        HttpPost
    };

    /// \brief Result returned by a transport policy.
    struct SyncTransportDecision {
        bool allowed = true;
        unsigned status_code = 0;
        std::string error;

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
    };

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

    private:
        std::vector<ISyncTransportPolicy*> m_policies;
    };

    /// \brief Snapshot of counters recorded by \c SyncTransportMetricsObserver.
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

    } // namespace detail

    /// \brief \c ISyncPeer wrapper that applies policy and observer hooks.
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

    /// \brief \c IHttpSyncClient wrapper that applies route-level policy.
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
            if (m_policy != nullptr) {
                const SyncTransportDecision decision =
                    m_policy->check_http_post(target, content_type, body);
                if (!decision.allowed) {
                    detail::notify_transport_rejected(
                        m_observer, SyncTransportOperation::HttpPost,
                        decision.error);
                    return make_http_error(decision);
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
        static HttpSyncResponse make_http_error(
                const SyncTransportDecision& decision) {
            HttpSyncResponse response;
            response.status_code =
                decision.status_code == 0 ? 403 : decision.status_code;
            response.content_type = "text/plain; charset=utf-8";
            response.error = decision.error.empty()
                ? "HTTP sync request rejected"
                : decision.error;
            response.body.assign(response.error.begin(), response.error.end());
            return response;
        }

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
