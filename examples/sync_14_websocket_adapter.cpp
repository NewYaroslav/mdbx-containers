/**
 * \ingroup mdbxc_examples
 * \brief WebSocket-shaped sync adapter without choosing a WebSocket framework.
 *
 * `WebSocketSyncPeer` implements `ISyncPeer` by sending one complete binary
 * request message through an `IWebSocketSyncChannel`. `WebSocketSyncServer`
 * handles the matching server-side binary message and returns one binary
 * response message.
 *
 * This example uses an in-process loopback channel so the code stays portable.
 * Replace `LoopbackWebSocketChannel::exchange_binary()` with
 * Simple-WebSocket-Server, Boost.Beast, uWebSockets, websocketpp, or another
 * framework. The
 * concrete binding should reassemble WebSocket fragments before calling the
 * server seam and should send the returned bytes as one binary message.
 *
 * Expected output:
 *   [websocket adapter] pull delivered 2 batch(es)
 *   [websocket adapter] push delivered 1 batch(es)
 *   OK: sync_14_websocket_adapter
 */

#include "sync_example_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

class LoopbackWebSocketChannel : public mdbxc::sync::IWebSocketSyncChannel {
public:
    explicit LoopbackWebSocketChannel(
            mdbxc::sync::WebSocketSyncServer& server)
        : m_server(server), m_cancel_count(0) {}

    std::vector<std::uint8_t> exchange_binary(
            const std::vector<std::uint8_t>& binary_message,
            const mdbxc::sync::CancellationToken& cancel_token) override {
        // A real WebSocket client would send `binary_message` as one binary
        // frame/message, wait for the response message, and map cancel_token
        // plus request_cancel() to the framework's close/interrupt primitive.
        (void)cancel_token;
        return m_server.handle_binary_message(binary_message);
    }

    void request_cancel() override {
        // A real implementation would interrupt the active exchange here.
        ++m_cancel_count;
    }

    std::size_t cancel_count() const { return m_cancel_count; }

private:
    mdbxc::sync::WebSocketSyncServer& m_server;
    std::size_t m_cancel_count;
};

} // namespace

int main() {
    const std::string primary_path = "sync_14_primary.mdbx";
    const std::string replica_path = "sync_14_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(0xF0);
    const mdbxc::sync::NodeId replica_node =
        sync_example::make_node(0xF1);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(0xF2);

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
        primary_ticks.insert_or_assign(1, "BTC/USD");
        primary_ticks.insert_or_assign(2, "ETH/USD");
        primary->detach_sync_capture();

        mdbxc::sync::WebSocketSyncServer primary_server(primary_engine);
        LoopbackWebSocketChannel primary_channel(primary_server);
        mdbxc::sync::WebSocketSyncPeer primary_peer(primary_channel);

        mdbxc::sync::PullRequest pull;
        pull.requester = replica_node;
        pull.db_id = db_id;
        pull.have = replica_engine.applied_cursor();
        pull.max_batches = 100;

        const mdbxc::sync::PullResponse pulled = primary_peer.pull(pull);
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
        std::printf("[websocket adapter] pull delivered %zu batch(es)\n",
                    pulled.batches.size());

        primary->attach_sync_capture(&capture);
        primary_ticks.insert_or_assign(3, "SOL/USD");
        primary->detach_sync_capture();

        mdbxc::sync::WebSocketSyncServer replica_server(replica_engine);
        LoopbackWebSocketChannel replica_channel(replica_server);
        mdbxc::sync::WebSocketSyncPeer replica_peer(replica_channel);

        const mdbxc::sync::PushRequest push =
            primary_engine.make_push_request(3, 0);
        const mdbxc::sync::PushResponse pushed = replica_peer.push(push);
        sync_example::require(pushed.ok,
                              "WebSocket push failed: " + pushed.error);
        std::printf("[websocket adapter] push delivered %zu batch(es)\n",
                    push.batches.size());

        replica_peer.request_cancel();
        sync_example::require(replica_channel.cancel_count() == 1u,
                              "request_cancel was not forwarded");

        mdbxc::KeyValueTable<int, std::string> replica_ticks(replica, "ticks");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 1,
                                      "ticks[1]") == "BTC/USD",
            "replica ticks[1] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 2,
                                      "ticks[2]") == "ETH/USD",
            "replica ticks[2] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 3,
                                      "ticks[3]") == "SOL/USD",
            "replica ticks[3] mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_14_websocket_adapter\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
