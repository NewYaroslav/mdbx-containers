/**
 * \ingroup mdbxc_examples
 * \brief WebSocket binding over Simple-WebSocket-Server and standalone Asio.
 *
 * This example binds the framework-neutral WebSocketSyncServer /
 * WebSocketSyncPeer seam to eidheim/Simple-WebSocket-Server with
 * chriskohlhoff standalone Asio. It sends complete binary WebSocket messages
 * encoded by TransportMessageCodec. The demo includes handshake bearer auth,
 * explicit session DB access, pre-decode message-size limits, and close-code
 * retry classification in client diagnostics.
 *
 * Build it explicitly:
 *
 *   cmake -S . -B tmp/build-ws-example \
 *       -DMDBXC_DEPS_MODE=BUNDLED \
 *       -DMDBXC_BUILD_TESTS=OFF \
 *       -DMDBXC_BUILD_EXAMPLES=ON \
 *       -DMDBXC_WEBSOCKET_SYNC_EXAMPLE=ON \
 *       -DCMAKE_CXX_STANDARD=11
 *   cmake --build tmp/build-ws-example \
 *       --target sync_17_websocket_simple_web_server
 *
 * Run the self-contained demo:
 *
 *   ./tmp/build-ws-example/bin/examples/sync_17_websocket_simple_web_server
 *   ./tmp/build-ws-example/bin/examples/sync_17_websocket_simple_web_server \
 *       127.0.0.1 18194
 *
 * Expected output:
 *
 *   [websocket server] listening on 127.0.0.1:<port>
 *   [websocket client] pull applied 2 batch(es)
 *   [websocket client] push sent 1 batch(es)
 *   [websocket client] request_cancel forwarded once
 *   OK: sync_17_websocket_simple_web_server
 */

#include <mdbx_containers/sync/transports/simple_web/WebSocketTransport.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sync_example_utils.hpp"

namespace {

const std::uint8_t kPrimaryNodeSeed = 0xC8;
const std::uint8_t kReplicaNodeSeed = 0xC9;
const std::uint8_t kDatabaseSeed = 0xCA;
const std::size_t kMaxWebSocketMessageBytes = 1024u * 1024u;

void seed_primary_rows(const std::shared_ptr<mdbxc::Connection>& db) {
    mdbxc::sync::ThreadLocalChangeAccumulator sink(db);
    mdbxc::KeyValueTable<int, std::string> quotes(db, "quotes");
    db->attach_sync_capture(&sink);
    quotes.insert_or_assign(1, "BTC/USD");
    quotes.insert_or_assign(2, "ETH/USD");
    db->detach_sync_capture();
}

void seed_replica_rows(const std::shared_ptr<mdbxc::Connection>& db) {
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

std::uint16_t parse_port(const char* text) {
    const int value = std::atoi(text);
    if (value <= 0 || value > 65535) {
        throw std::invalid_argument("port must be in 1..65535");
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 1 && argc != 3) {
        std::fprintf(stderr, "usage: %s [host port]\n", argv[0]);
        return 2;
    }

    const std::string host = argc == 3 ? argv[1] : "127.0.0.1";
    const std::uint16_t port = argc == 3 ? parse_port(argv[2]) : 18081;
    const std::string bearer_token = "replica-token";
    const std::string primary_path = "sync_17_primary.mdbx";
    const std::string replica_path = "sync_17_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    try {
        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica =
            sync_example::open(replica_path);

        const mdbxc::sync::NodeId primary_node =
            sync_example::make_node(kPrimaryNodeSeed);
        const mdbxc::sync::NodeId replica_node =
            sync_example::make_node(kReplicaNodeSeed);
        const mdbxc::sync::DbId db_id =
            sync_example::make_node(kDatabaseSeed);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_engine(replica);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        seed_primary_rows(primary);

        mdbxc::sync::CodecBounds bounds;
        bounds.max_transport_message_bytes = kMaxWebSocketMessageBytes;
        mdbxc::sync::WebSocketSyncServer ws_server(primary_engine, bounds);
        mdbxc::sync::TransportMessageSizePolicy ws_size(
            kMaxWebSocketMessageBytes);
        mdbxc::sync::WebSocketAuthenticatedNodeIdentityPolicy ws_identity(
            bounds);
        mdbxc::sync::CompositeSyncTransportPolicy ws_policy;
        ws_policy.add(ws_size);
        ws_policy.add(ws_identity);
        mdbxc::sync::WebSocketSyncServerMiddleware ws_middleware(
            ws_server, &ws_policy);
        mdbxc::sync::simple_web::WebSocketSyncListenerConfig
            listener_config;
        listener_config.host = host;
        listener_config.port = port;
        listener_config.bearer_token = bearer_token;
        listener_config.has_authenticated_node = true;
        listener_config.authenticated_node = replica_node;
        listener_config.db_access = mdbxc::sync::SyncDbAccess::only(db_id);
        mdbxc::sync::simple_web::WebSocketSyncListener listener(
            ws_middleware, listener_config);
        listener.start();
        std::printf("[websocket server] listening on %s:%u\n",
                    host.c_str(),
                    static_cast<unsigned>(listener.port()));

        mdbxc::sync::simple_web::WebSocketSyncChannel channel(
            host, port, bearer_token);
        mdbxc::sync::WebSocketSyncPeer peer(channel, bounds);

        mdbxc::sync::PullRequest pull;
        pull.requester = replica_node;
        pull.db_id = db_id;
        pull.have = replica_engine.applied_cursor();
        pull.max_batches = 100;

        const mdbxc::sync::PullResponse pulled = peer.pull(pull);
        sync_example::require(pulled.ok,
                              "WebSocket pull failed: " + pulled.error);

        mdbxc::sync::PushRequest local_apply;
        local_apply.sender = primary_node;
        local_apply.db_id = db_id;
        local_apply.batches = pulled.batches;
        const mdbxc::sync::PushResponse applied =
            replica_engine.handle_push(local_apply);
        sync_example::require(applied.ok,
                              "local apply failed: " + applied.error);
        std::printf("[websocket client] pull applied %zu batch(es)\n",
                    pulled.batches.size());

        seed_replica_rows(replica);
        const mdbxc::sync::PushRequest push =
            replica_engine.make_push_request(1, 0);
        const mdbxc::sync::PushResponse pushed = peer.push(push);
        sync_example::require(pushed.ok,
                              "WebSocket push failed: " + pushed.error);
        std::printf("[websocket client] push sent %zu batch(es)\n",
                    push.batches.size());

        peer.request_cancel();
        sync_example::require(channel.cancel_count() == 1u,
                              "request_cancel was not forwarded");
        std::printf("[websocket client] request_cancel forwarded once\n");

        require_quote(replica, 1, "BTC/USD");
        require_quote(replica, 2, "ETH/USD");
        require_quote(primary, 3, "SOL/USD");

        listener.stop();
        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_17_websocket_simple_web_server\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
