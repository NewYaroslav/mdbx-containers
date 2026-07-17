/**
 * \ingroup mdbxc_examples
 * \brief SyncWorker running through the HTTP-shaped transport adapter.
 *
 * This example is still in-process, but it keeps the same boundary a real HTTP
 * binding would use:
 *   - the replica owns SyncWorker and HttpSyncPeer;
 *   - the client sends TransportMessageCodec bytes plus adapter-local headers;
 *   - the primary-side HTTP middleware authenticates the bearer token as the
 *     replica NodeId before dispatching to HttpSyncServer;
 *   - the worker observer reports pages applied by the background loop.
 *
 * Expected output:
 *   [worker/http] page 1 applied ...
 *   [worker/http] page 2 applied ...
 *   OK: sync_16_worker_http_transport
 */

#include "sync_example_utils.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class ContextHttpClient : public mdbxc::sync::IHttpSyncClient {
public:
    ContextHttpClient(mdbxc::sync::HttpSyncServerMiddleware& server,
                      const std::string& bearer_token,
                      const std::string& remote_address)
        : m_server(server),
          m_bearer_token(bearer_token),
          m_remote_address(remote_address),
          m_post_count(0),
          m_cancel_count(0) {}

    mdbxc::sync::HttpSyncResponse post(
            const std::string& target,
            const std::string& content_type,
            const std::vector<std::uint8_t>& body,
            const mdbxc::sync::CancellationToken& cancel_token) override {
        // A concrete HTTP client would map `cancel_token` to request timeout
        // or socket cancellation. The token is local control state and is not
        // serialized by TransportMessageCodec.
        (void)cancel_token;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_post_count;
        }
        m_changed.notify_all();

        mdbxc::sync::HttpSyncRequest request;
        request.method = mdbxc::sync::HttpSyncRoutes::method_post();
        request.target = target;
        request.content_type = content_type;
        request.remote_address = m_remote_address;
        request.body = body;
        mdbxc::sync::http_add_header(
            request.headers, "Authorization",
            std::string("Bearer ") + m_bearer_token);
        return m_server.handle(request);
    }

    void request_cancel() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_cancel_count;
        }
        m_changed.notify_all();
    }

    bool wait_for_posts(std::size_t count,
                        std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this, count] { return m_post_count >= count; });
    }

    std::size_t cancel_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_count;
    }

private:
    mdbxc::sync::HttpSyncServerMiddleware& m_server;
    std::string m_bearer_token;
    std::string m_remote_address;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::size_t m_post_count;
    std::size_t m_cancel_count;
};

class ConsoleWorkerObserver : public mdbxc::sync::ISyncWorkerObserver {
public:
    explicit ConsoleWorkerObserver(const mdbxc::sync::NodeId& origin)
        : m_origin(origin), m_batches(0) {}

    void on_sync_worker_page_applied(
            const mdbxc::sync::SyncWorkerPageEvent& event) override {
        const unsigned long long origin_seq =
            static_cast<unsigned long long>(
                event.applied_cursor.last_seq_for(m_origin));
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_batches += event.batches_applied;
            std::printf(
                "[worker/http] page %zu applied %zu batch(es), "
                "origin_seq=%llu, has_more=%s\n",
                event.pages_pulled,
                event.batches_applied,
                origin_seq,
                event.has_more ? "true" : "false");
        }
        m_changed.notify_all();
    }

    bool wait_for_batches(std::size_t count,
                          std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this, count] { return m_batches >= count; });
    }

private:
    mdbxc::sync::NodeId m_origin;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::size_t m_batches;
};

} // namespace

int main() {
    const std::string primary_path = "sync_16_primary.mdbx";
    const std::string replica_path = "sync_16_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(0xB6);
    const mdbxc::sync::NodeId replica_node =
        sync_example::make_node(0xB7);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(0xB8);

    try {
        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica =
            sync_example::open(replica_path);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_engine(replica);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        mdbxc::sync::ThreadLocalChangeAccumulator capture(primary);
        mdbxc::KeyValueTable<int, std::string> primary_ticks(primary, "ticks");

        primary->attach_sync_capture(&capture);
        primary_ticks.insert_or_assign(1, "BTC/USD 65000");
        primary_ticks.insert_or_assign(2, "ETH/USD 3500");
        primary_ticks.insert_or_assign(3, "SOL/USD 180");
        primary->detach_sync_capture();

        mdbxc::sync::HttpBearerNodeIdentityPolicy identity;
        identity.allow_token_for_node("replica-token", replica_node);
        identity.allow_db_id_for_token("replica-token", db_id);

        mdbxc::sync::HttpSyncServer primary_server(primary_engine);
        mdbxc::sync::HttpSyncServerMiddleware guarded_server(
            primary_server, &identity);
        ContextHttpClient http_client(
            guarded_server, "replica-token", "127.0.0.1");
        mdbxc::sync::HttpSyncPeer http_peer(http_client);

        ConsoleWorkerObserver observer(primary_node);
        mdbxc::sync::SyncWorkerOptions options;
        options.max_batches = 2;
        options.idle_interval = std::chrono::milliseconds(10000);
        options.observer = &observer;

        mdbxc::sync::SyncWorker worker(replica_engine, http_peer, options);
        worker.start();
        sync_example::require(
            observer.wait_for_batches(3u, std::chrono::seconds(5)),
            "timed out waiting for HTTP worker replication");
        sync_example::require(
            http_client.wait_for_posts(2u, std::chrono::seconds(5)),
            "worker did not use paginated HTTP pull");
        worker.stop();
        sync_example::require(
            http_client.cancel_count() == 0u,
            "idle worker stop should not cancel HTTP transport");

        mdbxc::KeyValueTable<int, std::string> replica_ticks(replica, "ticks");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 1,
                                      "ticks[1]") == "BTC/USD 65000",
            "replica ticks[1] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 2,
                                      "ticks[2]") == "ETH/USD 3500",
            "replica ticks[2] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 3,
                                      "ticks[3]") == "SOL/USD 180",
            "replica ticks[3] mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_16_worker_http_transport\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
