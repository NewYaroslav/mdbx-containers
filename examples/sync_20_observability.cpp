/**
 * \ingroup mdbxc_examples
 * \brief SyncWorker observability with HTTP-shaped transport metadata.
 *
 * Expected output includes lines like:
 *
 *   [transport] request target=/mdbxc/sync/v1/pull request_id=sync-20-request
 *   [transport] rejected op=http_post error=rate limited once
 *   [worker] backoff retryable=yes retry_after=0s delay=0ms
 *   [worker] page=1 applied=1 progress=33%
 *   OK: sync_20_observability
 */

#include "sync_example_utils.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

const char* operation_name(mdbxc::sync::SyncTransportOperation operation) {
    switch (operation) {
        case mdbxc::sync::SyncTransportOperation::Pull:
            return "pull";
        case mdbxc::sync::SyncTransportOperation::Push:
            return "push";
        case mdbxc::sync::SyncTransportOperation::HttpPost:
            return "http_post";
        case mdbxc::sync::SyncTransportOperation::WebSocketMessage:
            return "websocket_message";
    }
    return "unknown";
}

class RejectFirstRequestPolicy : public mdbxc::sync::ISyncTransportPolicy {
public:
    RejectFirstRequestPolicy() : m_seen(false) {}

    mdbxc::sync::SyncTransportDecision check_http_request(
            const mdbxc::sync::HttpSyncRequest& request) override {
        (void)request;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_seen) {
            return mdbxc::sync::SyncTransportDecision::allow();
        }
        m_seen = true;
        mdbxc::sync::SyncTransportDecision decision =
            mdbxc::sync::SyncTransportDecision::reject(
                "rate limited once", 429);
        decision.add_response_header("Retry-After", "0");
        return decision;
    }

private:
    std::mutex m_mutex;
    bool m_seen;
};

class LoggingTransportObserver : public mdbxc::sync::ISyncTransportObserver {
public:
    void on_sync_transport_http_request(
            const mdbxc::sync::HttpSyncRequest& request) override {
        const mdbxc::sync::SyncTransportTraceContext trace =
            mdbxc::sync::http_sync_trace_context(request);
        std::printf(
            "[transport] request target=%s request_id=%s trace_id=%s\n",
            request.target.c_str(),
            trace.request_id.c_str(),
            trace.trace_id.c_str());
    }

    void on_sync_transport_rejected(
            mdbxc::sync::SyncTransportOperation operation,
            const std::string& error) override {
        std::printf("[transport] rejected op=%s error=%s\n",
                    operation_name(operation),
                    error.c_str());
    }

    void on_sync_transport_http_post_result(
            const std::string& target,
            const mdbxc::sync::HttpSyncResponse& response) override {
        std::printf("[transport] response target=%s status=%u\n",
                    target.c_str(),
                    response.status_code);
    }
};

class LoggingWorkerObserver : public mdbxc::sync::ISyncWorkerObserver {
public:
    LoggingWorkerObserver() : m_batches(0) {}

    void on_sync_worker_page_applied(
            const mdbxc::sync::SyncWorkerPageEvent& event) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_batches += event.batches_applied;
        }
        m_changed.notify_all();

        std::printf("[worker] page=%zu applied=%zu has_more=%s\n",
                    event.pages_pulled,
                    event.batches_applied,
                    event.has_more ? "yes" : "no");
    }

    void on_sync_worker_stage_changed(
            const mdbxc::sync::SyncWorkerStageEvent& event) override {
        if (event.stage != mdbxc::sync::SyncWorkerStage::ApplyFinished ||
            !event.progress.remote_tail_known ||
            event.progress.batches_total == 0) {
            return;
        }
        const double pct = event.progress.completion_ratio * 100.0;
        std::printf(
            "[worker] progress applied=%llu remaining=%llu total=%llu %.0f%%\n",
            static_cast<unsigned long long>(event.progress.batches_applied),
            static_cast<unsigned long long>(event.progress.batches_remaining),
            static_cast<unsigned long long>(event.progress.batches_total),
            pct);
    }

    void on_sync_worker_backoff(
            const mdbxc::sync::SyncWorkerRoundResult& result,
            std::chrono::milliseconds delay) override {
        if (result.retry_hint.has_retry_after) {
            std::printf(
                "[worker] backoff retryable=%s retry_after=%llus "
                "delay=%lldms error=%s\n",
                result.retry_hint.retryable ? "yes" : "no",
                static_cast<unsigned long long>(
                    result.retry_hint.retry_after_seconds),
                static_cast<long long>(delay.count()),
                result.error.c_str());
            return;
        }
        std::printf(
            "[worker] backoff retryable=%s retry_after=none "
            "delay=%lldms error=%s\n",
            result.retry_hint.retryable ? "yes" : "no",
            static_cast<long long>(delay.count()),
            result.error.c_str());
    }

    bool wait_for_batches(std::size_t count,
                          std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this, count] { return m_batches >= count; });
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::size_t m_batches;
};

class TraceHttpClient : public mdbxc::sync::IHttpSyncClient {
public:
    TraceHttpClient(mdbxc::sync::HttpSyncServerMiddleware& server,
                    const std::string& request_id,
                    const std::string& trace_id)
        : m_server(server),
          m_request_id(request_id),
          m_trace_id(trace_id) {}

    mdbxc::sync::HttpSyncResponse post(
            const std::string& target,
            const std::string& content_type,
            const std::vector<std::uint8_t>& body,
            const mdbxc::sync::CancellationToken& cancel_token) override {
        (void)cancel_token;
        mdbxc::sync::HttpSyncRequest request;
        request.method = mdbxc::sync::HttpSyncRoutes::method_post();
        request.target = target;
        request.content_type = content_type;
        request.body = body;
        mdbxc::sync::http_add_header(
            request.headers, mdbxc::sync::HttpSyncHeaders::request_id(),
            m_request_id);
        mdbxc::sync::http_add_header(
            request.headers, mdbxc::sync::HttpSyncHeaders::trace_id(),
            m_trace_id);
        return m_server.handle(request);
    }

private:
    mdbxc::sync::HttpSyncServerMiddleware& m_server;
    std::string m_request_id;
    std::string m_trace_id;
};

} // namespace

int main() {
    const std::string primary_path = "sync_20_primary.mdbx";
    const std::string replica_path = "sync_20_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    try {
        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica =
            sync_example::open(replica_path);

        const mdbxc::sync::NodeId primary_node =
            sync_example::make_node(0x20);
        const mdbxc::sync::NodeId replica_node =
            sync_example::make_node(0x21);
        const mdbxc::sync::DbId db_id = sync_example::make_node(0xD0);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_engine(replica);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        mdbxc::sync::ThreadLocalChangeAccumulator capture(primary);
        primary->attach_sync_capture(&capture);
        {
            mdbxc::KeyValueTable<int, std::string> ticks(primary, "ticks");
            ticks.insert_or_assign(1, "BTC/USD");
            ticks.insert_or_assign(2, "ETH/USD");
            ticks.insert_or_assign(3, "SOL/USD");
        }
        primary->detach_sync_capture();

        mdbxc::sync::HttpSyncServer primary_server(primary_engine);
        RejectFirstRequestPolicy rate_limit_once;
        LoggingTransportObserver transport_log;
        mdbxc::sync::HttpSyncServerMiddleware logged_server(
            primary_server, &rate_limit_once, &transport_log);
        TraceHttpClient http_client(
            logged_server, "sync-20-request", "sync-20-trace");
        mdbxc::sync::HttpSyncPeer http_peer(http_client);

        LoggingWorkerObserver worker_log;
        mdbxc::sync::SyncWorkerOptions options;
        options.max_batches = 1;
        options.initial_backoff = std::chrono::milliseconds(10000);
        options.max_backoff = std::chrono::milliseconds(10000);
        options.idle_interval = std::chrono::milliseconds(10000);
        options.observer = &worker_log;

        mdbxc::sync::SyncWorker worker(replica_engine, http_peer, options);
        worker.start();
        sync_example::require(
            worker_log.wait_for_batches(3u, std::chrono::seconds(5)),
            "timed out waiting for worker observability example");
        worker.stop();

        mdbxc::KeyValueTable<int, std::string> replica_ticks(replica, "ticks");
        sync_example::require(
            sync_example::kv_or_throw(
                replica, replica_ticks, 1, "replica tick 1") == "BTC/USD",
            "replica tick 1 mismatch");
        sync_example::require(
            sync_example::kv_or_throw(
                replica, replica_ticks, 3, "replica tick 3") == "SOL/USD",
            "replica tick 3 mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::puts("OK: sync_20_observability");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sync_20_observability failed: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
