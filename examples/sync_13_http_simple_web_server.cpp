/**
 * \ingroup mdbxc_examples
 * \brief HTTP binding over Simple-Web-Server and standalone Asio.
 *
 * This example is the first real socket-backed transport for the sync v0.1
 * DTOs. It binds the framework-neutral \c HttpSyncServer / \c HttpSyncPeer /
 * \c TransportMessageCodec seam to eidheim/Simple-Web-Server with
 * chriskohlhoff standalone Asio.
 *
 * Build it explicitly:
 *
 *   cmake -S . -B tmp/build-http-example \
 *       -DMDBXC_DEPS_MODE=BUNDLED \
 *       -DMDBXC_BUILD_TESTS=OFF \
 *       -DMDBXC_BUILD_EXAMPLES=ON \
 *       -DMDBXC_HTTP_SYNC_EXAMPLE=ON \
 *       -DCMAKE_CXX_STANDARD=11
 *   cmake --build tmp/build-http-example \
 *       --target sync_13_http_simple_web_server
 *
 * Run the self-contained demo:
 *
 *   ./tmp/build-http-example/bin/examples/sync_13_http_simple_web_server \
 *       demo 127.0.0.1 18080
 *   ./tmp/build-http-example/bin/examples/sync_13_http_simple_web_server \
 *       worker-demo 127.0.0.1 18080
 *
 * Manual two-process run:
 *
 *   ./sync_13_http_simple_web_server server 127.0.0.1 18080
 *   ./sync_13_http_simple_web_server client 127.0.0.1 18080
 *
 * Expected demo output:
 *
 *   [server] listening on 127.0.0.1:18080
 *   [client] HTTP pull applied 2 batch(es)
 *   [client] HTTP push sent 1 batch(es)
 *   [client] request_cancel forwarded once
 *   [client] metrics pull=1 push=1 http=2 rejected=0
 *   [demo] server received pushed quote: SOL/USD
 *   OK: sync_13_http_simple_web_server
 *   [worker-demo] HTTP page 1 applied ...
 *   [worker-demo] HTTP page 2 applied ...
 *   OK: sync_13_http_simple_web_server (worker-demo)
 */

#include <mdbx_containers/sync/transports/simple_web/HttpTransport.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sync_example_utils.hpp"

namespace {

// Demo-only seeds. They have no protocol meaning; they just make the two node
// ids and one db id deterministic and visibly distinct.
const std::uint8_t kServerNodeSeed = 0xA0;
const std::uint8_t kClientNodeSeed = 0xB0;
const std::uint8_t kDatabaseSeed = 0xA1;

void seed_server_rows(const std::shared_ptr<mdbxc::Connection>& db) {
    mdbxc::sync::ThreadLocalChangeAccumulator sink(db);
    mdbxc::KeyValueTable<int, std::string> quotes(db, "quotes");
    db->attach_sync_capture(&sink);
    quotes.insert_or_assign(1, "BTC/USD");
    quotes.insert_or_assign(2, "ETH/USD");
    db->detach_sync_capture();
}

void seed_client_rows(const std::shared_ptr<mdbxc::Connection>& db) {
    mdbxc::sync::ThreadLocalChangeAccumulator sink(db);
    mdbxc::KeyValueTable<int, std::string> quotes(db, "quotes");
    db->attach_sync_capture(&sink);
    quotes.insert_or_assign(3, "SOL/USD");
    db->detach_sync_capture();
}

void require_quote(const std::shared_ptr<mdbxc::Connection>& db,
                   int key,
                   const std::string& expected) {
    mdbxc::KeyValueTable<int, std::string> quotes(db, "quotes");
    const std::string actual =
        sync_example::kv_or_throw(db, quotes, key, "quotes");
    sync_example::require(actual == expected,
                          "quote mismatch for key " +
                          std::to_string(key));
}

class WorkerHttpObserver : public mdbxc::sync::ISyncWorkerObserver {
public:
    explicit WorkerHttpObserver(const mdbxc::sync::NodeId& origin)
        : m_origin(origin),
          m_batches(0) {}

    void on_sync_worker_page_applied(
            const mdbxc::sync::SyncWorkerPageEvent& event) override {
        const unsigned long long origin_seq =
            static_cast<unsigned long long>(
                event.applied_cursor.last_seq_for(m_origin));
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_batches += event.batches_applied;
            std::printf("[worker-demo] HTTP page %zu applied %zu batch(es), "
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
            [this, count]() { return m_batches >= count; });
    }

private:
    mdbxc::sync::NodeId m_origin;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::size_t m_batches;
};

void run_client_session(const std::shared_ptr<mdbxc::Connection>& db,
                        mdbxc::sync::SyncEngine& engine,
                        const std::string& host,
                        std::uint16_t port) {
    mdbxc::sync::simple_web::HttpSyncClient raw_client(
        host, port, std::chrono::seconds(2));

    mdbxc::sync::HttpRouteAllowListPolicy routes;
    routes.allow_target(mdbxc::sync::HttpSyncRoutes::pull_target());
    routes.allow_target(mdbxc::sync::HttpSyncRoutes::push_target());

    mdbxc::sync::FixedBudgetSyncTransportPolicy http_budget(
        mdbxc::sync::FixedBudgetSyncTransportPolicy::unlimited_budget(),
        mdbxc::sync::FixedBudgetSyncTransportPolicy::unlimited_budget(),
        8);
    mdbxc::sync::CompositeSyncTransportPolicy http_policy;
    http_policy.add(routes);
    http_policy.add(http_budget);

    mdbxc::sync::SyncTransportMetricsObserver metrics;
    mdbxc::sync::HttpSyncClientMiddleware http_client(
        raw_client, &http_policy, &metrics);
    mdbxc::sync::HttpSyncPeer http_peer(http_client);

    mdbxc::sync::FixedBudgetSyncTransportPolicy peer_budget(4, 4);
    mdbxc::sync::SyncPeerMiddleware peer(
        http_peer, &peer_budget, &metrics);

    const mdbxc::sync::NodeId server_node =
        sync_example::make_node(kServerNodeSeed);
    const mdbxc::sync::NodeId client_node =
        sync_example::make_node(kClientNodeSeed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(kDatabaseSeed);

    mdbxc::sync::PullRequest pull;
    pull.requester = client_node;
    pull.db_id = db_id;
    pull.have = engine.applied_cursor();
    pull.max_batches = 100;

    const mdbxc::sync::PullResponse pulled = peer.pull(pull);
    sync_example::require(pulled.ok,
                          "HTTP pull failed: " + pulled.error);

    mdbxc::sync::PushRequest local_apply;
    local_apply.sender = server_node;
    local_apply.db_id = db_id;
    local_apply.batches = pulled.batches;
    const mdbxc::sync::PushResponse applied =
        engine.handle_push(local_apply);
    sync_example::require(applied.ok,
                          "client local apply failed: " + applied.error);
    std::printf("[client] HTTP pull applied %zu batch(es)\n",
                pulled.batches.size());

    seed_client_rows(db);
    const mdbxc::sync::PushRequest push =
        engine.make_push_request(1, 0);
    const mdbxc::sync::PushResponse pushed = peer.push(push);
    sync_example::require(pushed.ok,
                          "HTTP push failed: " + pushed.error);
    std::printf("[client] HTTP push sent %zu batch(es)\n",
                push.batches.size());

    peer.request_cancel();
    sync_example::require(raw_client.cancel_count() == 1u,
                          "request_cancel was not forwarded");
    std::printf("[client] request_cancel forwarded once\n");

    require_quote(db, 1, "BTC/USD");
    require_quote(db, 2, "ETH/USD");

    const mdbxc::sync::SyncTransportMetricsSnapshot snapshot =
        metrics.snapshot();
    std::printf("[client] metrics pull=%llu push=%llu http=%llu "
                "rejected=%llu\n",
                static_cast<unsigned long long>(snapshot.pull_calls),
                static_cast<unsigned long long>(snapshot.push_calls),
                static_cast<unsigned long long>(snapshot.http_post_calls),
                static_cast<unsigned long long>(snapshot.rejected_calls));
}

int run_demo(const std::string& host, std::uint16_t port) {
    const std::string server_path = "sync_13_server_demo.mdbx";
    const std::string client_path = "sync_13_client_demo.mdbx";
    sync_example::cleanup(server_path);
    sync_example::cleanup(client_path);

    std::shared_ptr<mdbxc::Connection> server_db =
        sync_example::open(server_path);
    std::shared_ptr<mdbxc::Connection> client_db =
        sync_example::open(client_path);

    const mdbxc::sync::NodeId server_node =
        sync_example::make_node(kServerNodeSeed);
    const mdbxc::sync::NodeId client_node =
        sync_example::make_node(kClientNodeSeed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(kDatabaseSeed);

    mdbxc::sync::SyncEngine server_engine(server_db);
    mdbxc::sync::SyncEngine client_engine(client_db);
    server_engine.initialize_local_identity(server_node, db_id);
    client_engine.initialize_local_identity(client_node, db_id);

    seed_server_rows(server_db);

    mdbxc::sync::HttpSyncServer handler(server_engine);
    mdbxc::sync::HttpRouteAllowListPolicy route_policy;
    route_policy.allow_target(mdbxc::sync::HttpSyncRoutes::pull_target());
    route_policy.allow_target(mdbxc::sync::HttpSyncRoutes::push_target());

    mdbxc::sync::simple_web::HttpSyncListenerConfig listener_config;
    listener_config.host = host;
    listener_config.port = port;
    mdbxc::sync::simple_web::HttpSyncListener listener(
        handler, listener_config, &route_policy);
    listener.start();
    std::printf("[server] listening on %s:%u\n",
                host.c_str(),
                static_cast<unsigned>(listener.port()));

    run_client_session(client_db, client_engine, host, port);
    require_quote(server_db, 3, "SOL/USD");
    std::printf("[demo] server received pushed quote: SOL/USD\n");

    listener.stop();
    sync_example::disconnect_and_cleanup(server_db, server_path);
    sync_example::disconnect_and_cleanup(client_db, client_path);
    std::printf("OK: sync_13_http_simple_web_server\n");
    return 0;
}

int run_worker_demo(const std::string& host, std::uint16_t port) {
    const std::string server_path = "sync_13_worker_server.mdbx";
    const std::string client_path = "sync_13_worker_client.mdbx";
    sync_example::cleanup(server_path);
    sync_example::cleanup(client_path);

    std::shared_ptr<mdbxc::Connection> server_db =
        sync_example::open(server_path);
    std::shared_ptr<mdbxc::Connection> client_db =
        sync_example::open(client_path);

    const mdbxc::sync::NodeId server_node =
        sync_example::make_node(kServerNodeSeed);
    const mdbxc::sync::NodeId client_node =
        sync_example::make_node(kClientNodeSeed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(kDatabaseSeed);

    mdbxc::sync::SyncEngine server_engine(server_db);
    mdbxc::sync::SyncEngine client_engine(client_db);
    server_engine.initialize_local_identity(server_node, db_id);
    client_engine.initialize_local_identity(client_node, db_id);

    seed_server_rows(server_db);
    {
        mdbxc::sync::ThreadLocalChangeAccumulator sink(server_db);
        mdbxc::KeyValueTable<int, std::string> quotes(server_db, "quotes");
        server_db->attach_sync_capture(&sink);
        quotes.insert_or_assign(3, "SOL/USD");
        server_db->detach_sync_capture();
    }

    mdbxc::sync::HttpSyncServer handler(server_engine);
    mdbxc::sync::HttpRouteAllowListPolicy route_policy;
    route_policy.allow_target(mdbxc::sync::HttpSyncRoutes::pull_target());
    route_policy.allow_target(mdbxc::sync::HttpSyncRoutes::push_target());

    mdbxc::sync::TransportMessageSizePolicy size_policy(1024u * 1024u);
    mdbxc::sync::HttpBearerNodeIdentityPolicy identity_policy;
    identity_policy.allow_token_for_node("replica-token", client_node);
    identity_policy.allow_db_id_for_token("replica-token", db_id);

    mdbxc::sync::CompositeSyncTransportPolicy server_policy;
    server_policy.add(route_policy);
    server_policy.add(size_policy);
    server_policy.add(identity_policy);

    mdbxc::sync::simple_web::HttpSyncListenerConfig listener_config;
    listener_config.host = host;
    listener_config.port = port;
    mdbxc::sync::simple_web::HttpSyncListener listener(
        handler, listener_config, &server_policy);
    listener.start();

    mdbxc::sync::simple_web::HttpSyncClient raw_client(
        host, port, std::chrono::seconds(2), "replica-token");
    mdbxc::sync::SyncTransportMetricsObserver metrics;
    mdbxc::sync::HttpSyncClientMiddleware http_client(
        raw_client, nullptr, &metrics);
    mdbxc::sync::HttpSyncPeer http_peer(http_client);

    WorkerHttpObserver observer(server_node);
    mdbxc::sync::SyncWorkerOptions options;
    options.max_batches = 2;
    options.idle_interval = std::chrono::milliseconds(10000);
    options.observer = &observer;

    mdbxc::sync::SyncWorker worker(client_engine, http_peer, options);
    worker.start();
    sync_example::require(
        observer.wait_for_batches(3u, std::chrono::seconds(5)),
        "timed out waiting for worker HTTP replication");
    worker.stop();

    const mdbxc::sync::SyncTransportMetricsSnapshot snapshot =
        metrics.snapshot();
    sync_example::require(snapshot.http_post_calls >= 2u,
                          "worker demo expected paginated HTTP pulls");

    require_quote(client_db, 1, "BTC/USD");
    require_quote(client_db, 2, "ETH/USD");
    require_quote(client_db, 3, "SOL/USD");
    std::printf("[worker-demo] HTTP posts=%llu rejected=%llu\n",
                static_cast<unsigned long long>(snapshot.http_post_calls),
                static_cast<unsigned long long>(snapshot.rejected_calls));

    listener.stop();
    sync_example::disconnect_and_cleanup(server_db, server_path);
    sync_example::disconnect_and_cleanup(client_db, client_path);
    std::printf("OK: sync_13_http_simple_web_server (worker-demo)\n");
    return 0;
}

int run_server(const std::string& host, std::uint16_t port) {
    const std::string path = "sync_13_server.mdbx";
    sync_example::cleanup(path);
    std::shared_ptr<mdbxc::Connection> db = sync_example::open(path);

    const mdbxc::sync::NodeId server_node =
        sync_example::make_node(kServerNodeSeed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(kDatabaseSeed);

    mdbxc::sync::SyncEngine engine(db);
    engine.initialize_local_identity(server_node, db_id);
    seed_server_rows(db);

    mdbxc::sync::HttpSyncServer handler(engine);
    mdbxc::sync::HttpRouteAllowListPolicy route_policy;
    route_policy.allow_target(mdbxc::sync::HttpSyncRoutes::pull_target());
    route_policy.allow_target(mdbxc::sync::HttpSyncRoutes::push_target());

    mdbxc::sync::simple_web::HttpSyncListenerConfig listener_config;
    listener_config.host = host;
    listener_config.port = port;
    mdbxc::sync::simple_web::HttpSyncListener listener(
        handler, listener_config, &route_policy);
    listener.start();
    std::printf("[server] listening on %s:%u\n",
                host.c_str(),
                static_cast<unsigned>(listener.port()));

    std::printf("[server] press Enter to stop\n");
    std::string line;
    std::getline(std::cin, line);

    listener.stop();
    sync_example::disconnect_and_cleanup(db, path);
    std::printf("OK: sync_13_http_simple_web_server (server)\n");
    return 0;
}

int run_client(const std::string& host, std::uint16_t port) {
    const std::string path = "sync_13_client.mdbx";
    sync_example::cleanup(path);
    std::shared_ptr<mdbxc::Connection> db = sync_example::open(path);

    const mdbxc::sync::NodeId client_node =
        sync_example::make_node(kClientNodeSeed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(kDatabaseSeed);

    mdbxc::sync::SyncEngine engine(db);
    engine.initialize_local_identity(client_node, db_id);

    run_client_session(db, engine, host, port);

    sync_example::disconnect_and_cleanup(db, path);
    std::printf("OK: sync_13_http_simple_web_server (client)\n");
    return 0;
}

std::uint16_t parse_port(const char* text) {
    const int value = std::atoi(text);
    if (value <= 0 || value > 65535) {
        throw std::invalid_argument("port must be in 1..65535");
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            std::fprintf(stderr,
                "usage: %s (demo|worker-demo|server|client) <host> <port>\n",
                argv[0]);
            return 2;
        }

        const std::string mode = argv[1];
        const std::string host = argv[2];
        const std::uint16_t port = parse_port(argv[3]);

        if (mode == "demo") {
            return run_demo(host, port);
        }
        if (mode == "worker-demo") {
            return run_worker_demo(host, port);
        }
        if (mode == "server") {
            return run_server(host, port);
        }
        if (mode == "client") {
            return run_client(host, port);
        }

        std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 2;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
