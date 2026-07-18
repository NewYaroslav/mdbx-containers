/**
 * \ingroup mdbxc_examples
 * \brief Production-facing transport wrapper shape.
 *
 * The sync core deliberately does not know about TLS, bearer tokens, retry
 * headers, structured logging, or deployment shutdown rules. A production
 * service adds those concerns around an ISyncPeer implementation.
 *
 * This example keeps the socket side in-process with DirectSyncPeer so it runs
 * everywhere, but the wrapper mirrors the places where a real HTTP/WSS adapter
 * would attach:
 *   - endpoint and TLS policy;
 *   - token lookup / rotation outside PullRequest and PushRequest DTOs;
 *   - request ids and structured log fields;
 *   - best-effort transport cancellation;
 *   - SyncWorker observer callbacks for status and progress reporting.
 *
 * Expected output:
 *   [sync.transport] event=pull.start ...
 *   [sync.worker] stage=...
 *   [sync.app] token rotated
 *   OK: sync_20_transport_production_wrapper
 */

#include "sync_example_utils.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

namespace {

const std::uint8_t kPrimaryNodeSeed = 0xD1;
const std::uint8_t kReplicaNodeSeed = 0xD2;
const std::uint8_t kDatabaseSeed = 0xD3;

const char* stage_name(mdbxc::sync::SyncWorkerStage stage) {
    switch (stage) {
        case mdbxc::sync::SyncWorkerStage::RoundStarted:
            return "round-started";
        case mdbxc::sync::SyncWorkerStage::PullStarted:
            return "pull-started";
        case mdbxc::sync::SyncWorkerStage::PullFinished:
            return "pull-finished";
        case mdbxc::sync::SyncWorkerStage::ApplyStarted:
            return "apply-started";
        case mdbxc::sync::SyncWorkerStage::ApplyFinished:
            return "apply-finished";
        case mdbxc::sync::SyncWorkerStage::RoundCompleted:
            return "round-completed";
        case mdbxc::sync::SyncWorkerStage::BackoffStarted:
            return "backoff-started";
    }
    return "unknown";
}

struct TransportRuntimeConfig {
    std::string endpoint = "demo://primary";
    bool tls_enabled = true;
    std::size_t max_body_bytes = 4u * 1024u * 1024u;
};

class RotatingBearerTokenProvider {
public:
    explicit RotatingBearerTokenProvider(const std::string& initial_token)
        : m_token(initial_token), m_generation(1) {}

    std::string current_token() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_token;
    }

    std::size_t generation() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generation;
    }

    void rotate(const std::string& next_token) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_token = next_token;
        ++m_generation;
    }

private:
    mutable std::mutex m_mutex;
    std::string m_token;
    std::size_t m_generation;
};

class StructuredLogger {
public:
    void log_transport(const char* event,
                       std::size_t request_id,
                       const TransportRuntimeConfig& config,
                       std::size_t token_generation,
                       bool ok,
                       std::size_t batches,
                       bool has_more,
                       const std::string& error) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::printf(
            "[sync.transport] event=%s request_id=%zu endpoint=%s tls=%s "
            "max_body=%zu token_generation=%zu ok=%s batches=%zu "
            "has_more=%s error=%s\n",
            event,
            request_id,
            config.endpoint.c_str(),
            config.tls_enabled ? "true" : "false",
            config.max_body_bytes,
            token_generation,
            ok ? "true" : "false",
            batches,
            has_more ? "true" : "false",
            error.empty() ? "-" : error.c_str());
    }

    void log_worker_stage(const mdbxc::sync::SyncWorkerStageEvent& event) const {
        if (event.stage != mdbxc::sync::SyncWorkerStage::PullStarted &&
            event.stage != mdbxc::sync::SyncWorkerStage::ApplyFinished &&
            event.stage != mdbxc::sync::SyncWorkerStage::RoundCompleted) {
            return;
        }

        const double progress_pct =
            event.progress.remote_tail_known
                ? event.progress.completion_ratio * 100.0
                : 0.0;
        std::lock_guard<std::mutex> lock(m_mutex);
        std::printf(
            "[sync.worker] stage=%s pages=%zu page_batches=%zu "
            "applied=%zu remaining=%llu progress=%.1f%% ok=%s\n",
            stage_name(event.stage),
            event.pages_pulled,
            event.batches_in_page,
            event.batches_applied,
            static_cast<unsigned long long>(
                event.progress.batches_remaining),
            progress_pct,
            event.ok ? "true" : "false");
    }

    void log_cancel(std::size_t count) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::printf("[sync.transport] event=cancel count=%zu\n", count);
    }

private:
    mutable std::mutex m_mutex;
};

class ProductionPeerWrapper : public mdbxc::sync::ISyncPeer {
public:
    ProductionPeerWrapper(mdbxc::sync::ISyncPeer& inner,
                          RotatingBearerTokenProvider& tokens,
                          const TransportRuntimeConfig& config,
                          const StructuredLogger& logger)
        : m_inner(inner),
          m_tokens(tokens),
          m_config(config),
          m_logger(logger),
          m_request_id(0),
          m_cancel_count(0) {}

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        const std::size_t request_id = next_request_id();
        const std::size_t token_generation = m_tokens.generation();
        const std::string token = m_tokens.current_token();
        (void)token;

        // The token is adapter-local metadata. A real HTTP/WSS binding would
        // put it in an Authorization header or handshake context, not inside
        // PullRequest.
        m_logger.log_transport("pull.start",
                               request_id,
                               m_config,
                               token_generation,
                               true,
                               0,
                               false,
                               std::string());
        const mdbxc::sync::PullResponse response = m_inner.pull(request);
        m_logger.log_transport("pull.finish",
                               request_id,
                               m_config,
                               token_generation,
                               response.ok,
                               response.batches.size(),
                               response.has_more,
                               response.error);
        return response;
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        return m_inner.push(request);
    }

    void request_cancel() override {
        const std::size_t count = increment_cancel_count();
        m_inner.request_cancel();
        m_logger.log_cancel(count);
    }

    std::size_t cancel_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_count;
    }

private:
    std::size_t next_request_id() {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_request_id;
        return m_request_id;
    }

    std::size_t increment_cancel_count() {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_cancel_count;
        return m_cancel_count;
    }

    mdbxc::sync::ISyncPeer& m_inner;
    RotatingBearerTokenProvider& m_tokens;
    TransportRuntimeConfig m_config;
    const StructuredLogger& m_logger;
    mutable std::mutex m_mutex;
    std::size_t m_request_id;
    std::size_t m_cancel_count;
};

class LoggingWorkerObserver : public mdbxc::sync::ISyncWorkerObserver {
public:
    explicit LoggingWorkerObserver(const StructuredLogger& logger)
        : m_logger(logger), m_batches(0) {}

    void on_sync_worker_stage_changed(
            const mdbxc::sync::SyncWorkerStageEvent& event) override {
        m_logger.log_worker_stage(event);
    }

    void on_sync_worker_page_applied(
            const mdbxc::sync::SyncWorkerPageEvent& event) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_batches += event.batches_applied;
        }
        m_changed.notify_all();
    }

    bool wait_for_batches(std::size_t count,
                          std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock,
            timeout,
            [this, count] { return m_batches >= count; });
    }

private:
    const StructuredLogger& m_logger;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::size_t m_batches;
};

void write_quote(const std::shared_ptr<mdbxc::Connection>& db,
                 mdbxc::KeyValueTable<int, std::string>& quotes,
                 mdbxc::sync::ThreadLocalChangeAccumulator& capture,
                 int key,
                 const std::string& value) {
    db->attach_sync_capture(&capture);
    quotes.insert_or_assign(key, value);
    db->detach_sync_capture();
}

} // namespace

int main() {
    const std::string primary_path = "sync_20_primary.mdbx";
    const std::string replica_path = "sync_20_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    try {
        const mdbxc::sync::NodeId primary_node =
            sync_example::make_node(kPrimaryNodeSeed);
        const mdbxc::sync::NodeId replica_node =
            sync_example::make_node(kReplicaNodeSeed);
        const mdbxc::sync::DbId db_id =
            sync_example::make_node(kDatabaseSeed);

        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica =
            sync_example::open(replica_path);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_engine(replica);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        mdbxc::sync::ThreadLocalChangeAccumulator capture(primary);
        mdbxc::KeyValueTable<int, std::string> primary_quotes(primary,
                                                              "quotes");

        write_quote(primary, primary_quotes, capture, 1, "BTC/USD");
        write_quote(primary, primary_quotes, capture, 2, "ETH/USD");

        mdbxc::sync::DirectSyncPeer direct_peer(&primary_engine);
        RotatingBearerTokenProvider tokens("token-generation-1");
        TransportRuntimeConfig transport_config;
        StructuredLogger logger;
        ProductionPeerWrapper production_peer(
            direct_peer, tokens, transport_config, logger);

        LoggingWorkerObserver observer(logger);
        mdbxc::sync::SyncWorkerOptions options;
        options.max_batches = 1;
        options.idle_interval = std::chrono::milliseconds(25);
        options.initial_backoff = std::chrono::milliseconds(50);
        options.max_backoff = std::chrono::milliseconds(200);
        options.observer = &observer;

        mdbxc::sync::SyncWorker worker(
            replica_engine, production_peer, options);
        worker.start();
        sync_example::require(
            observer.wait_for_batches(2, std::chrono::seconds(5)),
            "timed out waiting for initial catch-up");

        tokens.rotate("token-generation-2");
        std::puts("[sync.app] token rotated");

        write_quote(primary, primary_quotes, capture, 3, "SOL/USD");
        sync_example::require(
            observer.wait_for_batches(3, std::chrono::seconds(5)),
            "timed out waiting after token rotation");

        worker.request_stop();
        production_peer.request_cancel();
        worker.join();
        sync_example::require(production_peer.cancel_count() == 1u,
                              "transport cancellation was not logged");

        mdbxc::KeyValueTable<int, std::string> replica_quotes(replica,
                                                              "quotes");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_quotes, 1,
                                      "quotes[1]") == "BTC/USD",
            "quotes[1] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_quotes, 2,
                                      "quotes[2]") == "ETH/USD",
            "quotes[2] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_quotes, 3,
                                      "quotes[3]") == "SOL/USD",
            "quotes[3] mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::puts("OK: sync_20_transport_production_wrapper");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
