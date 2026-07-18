#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <cstdio>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId node{};
    for (int i = 0; i < 16; ++i) {
        node[i] = static_cast<std::uint8_t>(seed + i);
    }
    return node;
}

void cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-lck").c_str());
}

mdbxc::Config config(const std::string& path) {
    mdbxc::Config cfg;
    cfg.pathname = path;
    cfg.no_subdir = true;
    cfg.max_dbs = 32;
    return cfg;
}

std::shared_ptr<mdbxc::Connection> open_db(const std::string& path) {
    return mdbxc::Connection::create(config(path));
}

void require_true(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string get_value(const std::shared_ptr<mdbxc::Connection>& conn,
                      mdbxc::KeyValueTable<int, std::string>& table,
                      int key) {
    std::string out;
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    if (!table.try_get(key, out, txn.handle())) {
        throw std::runtime_error("missing replicated value");
    }
    return out;
}

class LoopbackWebSocketChannel : public mdbxc::sync::IWebSocketSyncChannel {
public:
    explicit LoopbackWebSocketChannel(
            mdbxc::sync::WebSocketSyncServer& server)
        : m_server(server),
          m_exchange_count(0),
          m_cancel_count(0),
          m_last_token_cancellable(false),
          m_last_type(mdbxc::sync::TransportMessageType::PullRequest) {}

    std::vector<std::uint8_t> exchange_binary(
            const std::vector<std::uint8_t>& binary_message,
            const mdbxc::sync::CancellationToken& cancel_token) override {
        ++m_exchange_count;
        m_last_token_cancellable = cancel_token.can_be_cancelled();
        m_last_type =
            mdbxc::sync::TransportMessageCodec::peek_message_type(
                binary_message);
        return m_server.handle_binary_message(binary_message);
    }

    void request_cancel() override {
        ++m_cancel_count;
    }

    std::size_t exchange_count() const { return m_exchange_count; }
    std::size_t cancel_count() const { return m_cancel_count; }
    bool last_token_cancellable() const { return m_last_token_cancellable; }
    mdbxc::sync::TransportMessageType last_type() const {
        return m_last_type;
    }

private:
    mdbxc::sync::WebSocketSyncServer& m_server;
    std::size_t m_exchange_count;
    std::size_t m_cancel_count;
    bool m_last_token_cancellable;
    mdbxc::sync::TransportMessageType m_last_type;
};

void test_websocket_peer_pull_and_push_roundtrip() {
    const std::string primary_path = "test_websocket_transport_primary.mdbx";
    const std::string replica_path = "test_websocket_transport_replica.mdbx";
    cleanup(primary_path);
    cleanup(replica_path);

    std::shared_ptr<mdbxc::Connection> primary = open_db(primary_path);
    std::shared_ptr<mdbxc::Connection> replica = open_db(replica_path);

    const mdbxc::sync::NodeId primary_node = make_node(0x10);
    const mdbxc::sync::NodeId replica_node = make_node(0x20);
    const mdbxc::sync::DbId db_id = make_node(0xD0);

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

    mdbxc::sync::CancellationSource pull_cancel;
    mdbxc::sync::PullRequest pull;
    pull.requester = replica_node;
    pull.db_id = db_id;
    pull.have = replica_engine.applied_cursor();
    pull.max_batches = 100;
    pull.cancel_token = pull_cancel.token();

    const mdbxc::sync::PullResponse pulled = primary_peer.pull(pull);
    require_true(pulled.ok, "WebSocket pull failed: " + pulled.error);
    require_true(pulled.batches.size() == 2u,
                 "WebSocket pull expected two batches");
    require_true(primary_channel.last_type() ==
                     mdbxc::sync::TransportMessageType::PullRequest,
                 "WebSocket pull sent wrong message type");
    require_true(primary_channel.exchange_count() == 1u,
                 "WebSocket pull exchange count mismatch");
    require_true(primary_channel.last_token_cancellable(),
                 "WebSocket channel did not receive cancellation token");

    mdbxc::sync::PushRequest local_apply;
    local_apply.sender = primary_node;
    local_apply.db_id = db_id;
    local_apply.batches = pulled.batches;
    const mdbxc::sync::PushResponse applied =
        replica_engine.handle_push(local_apply);
    require_true(applied.ok, "local apply failed: " + applied.error);

    primary->attach_sync_capture(&capture);
    primary_ticks.insert_or_assign(3, "SOL/USD");
    primary->detach_sync_capture();

    mdbxc::sync::WebSocketSyncServer replica_server(replica_engine);
    LoopbackWebSocketChannel replica_channel(replica_server);
    mdbxc::sync::WebSocketSyncPeer replica_peer(replica_channel);

    mdbxc::sync::PushRequest push = primary_engine.make_push_request(3, 0);
    require_true(push.batches.size() == 1u,
                 "WebSocket push expected one batch");
    const mdbxc::sync::PushResponse pushed = replica_peer.push(push);
    require_true(pushed.ok, "WebSocket push failed: " + pushed.error);
    require_true(pushed.receiver_have.last_seq_for(primary_node) == 3u,
                 "WebSocket push cursor mismatch");
    require_true(replica_channel.last_type() ==
                     mdbxc::sync::TransportMessageType::PushRequest,
                 "WebSocket push sent wrong message type");
    require_true(replica_channel.exchange_count() == 1u,
                 "WebSocket push exchange count mismatch");

    replica_peer.request_cancel();
    require_true(replica_channel.cancel_count() == 1u,
                 "WebSocket peer did not forward request_cancel()");

    mdbxc::KeyValueTable<int, std::string> replica_ticks(replica, "ticks");
    require_true(get_value(replica, replica_ticks, 1) == "BTC/USD",
                 "replica value 1 mismatch");
    require_true(get_value(replica, replica_ticks, 2) == "ETH/USD",
                 "replica value 2 mismatch");
    require_true(get_value(replica, replica_ticks, 3) == "SOL/USD",
                 "replica value 3 mismatch");

    primary->disconnect();
    replica->disconnect();
    cleanup(primary_path);
    cleanup(replica_path);
}

void test_websocket_server_rejects_response_messages() {
    const std::string path = "test_websocket_transport_reject.mdbx";
    cleanup(path);

    std::shared_ptr<mdbxc::Connection> db = open_db(path);
    mdbxc::sync::SyncEngine engine(db);
    engine.initialize_local_identity(make_node(0x30), make_node(0xD1));
    mdbxc::sync::WebSocketSyncServer server(engine);

    const std::vector<std::uint8_t> response_message =
        mdbxc::sync::TransportMessageCodec::encode_pull_response(
            mdbxc::sync::PullResponse());

    bool caught = false;
    try {
        (void)server.handle_binary_message(response_message);
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()).find("response message") !=
                 std::string::npos;
    }
    require_true(caught,
                 "WebSocket server must reject response messages");

    db->disconnect();
    cleanup(path);
}

void test_websocket_server_rejects_malformed_messages() {
    const std::string path = "test_websocket_transport_malformed.mdbx";
    cleanup(path);

    std::shared_ptr<mdbxc::Connection> db = open_db(path);
    mdbxc::sync::SyncEngine engine(db);
    engine.initialize_local_identity(make_node(0x40), make_node(0xD2));
    mdbxc::sync::WebSocketSyncServer server(engine);

    const std::uint8_t malformed_byte_1 = 0x01;
    const std::uint8_t malformed_byte_2 = 0x02;
    std::vector<std::uint8_t> malformed_message;
    malformed_message.push_back(malformed_byte_1);
    malformed_message.push_back(malformed_byte_2);

    bool caught = false;
    try {
        (void)server.handle_binary_message(malformed_message);
    } catch (const std::runtime_error&) {
        caught = true;
    }

    db->disconnect();
    cleanup(path);

    require_true(caught,
                 "WebSocket server must reject malformed messages");
}

void test_websocket_authenticated_node_policy() {
    const mdbxc::sync::NodeId node_a = make_node(0x11);
    const mdbxc::sync::NodeId node_b = make_node(0x22);
    const mdbxc::sync::DbId db_a = make_node(0xD1);
    const mdbxc::sync::DbId db_b = make_node(0xD2);

    mdbxc::sync::WebSocketAuthenticatedNodeIdentityPolicy policy;
    mdbxc::sync::WebSocketSyncRequestContext context;
    context.has_authenticated_node = true;
    context.authenticated_node = node_a;
    context.allowed_dbs.insert(db_a);

    mdbxc::sync::PullRequest pull;
    pull.requester = node_a;
    pull.db_id = db_a;
    context.binary_message =
        mdbxc::sync::TransportMessageCodec::encode_pull_request(pull);

    mdbxc::sync::SyncTransportDecision decision =
        policy.check_websocket_message(context);
    require_true(decision.allowed,
                 "matching WebSocket node identity was rejected");

    pull.requester = node_b;
    context.binary_message =
        mdbxc::sync::TransportMessageCodec::encode_pull_request(pull);
    decision = policy.check_websocket_message(context);
    require_true(!decision.allowed && decision.status_code == 1008,
                 "WebSocket requester mismatch was not rejected");

    pull.requester = node_a;
    pull.db_id = db_b;
    context.binary_message =
        mdbxc::sync::TransportMessageCodec::encode_pull_request(pull);
    decision = policy.check_websocket_message(context);
    require_true(!decision.allowed && decision.status_code == 1008,
                 "WebSocket db_id mismatch was not rejected");

    mdbxc::sync::PushRequest push;
    push.sender = node_b;
    push.db_id = db_a;
    context.binary_message =
        mdbxc::sync::TransportMessageCodec::encode_push_request(push);
    decision = policy.check_websocket_message(context);
    require_true(!decision.allowed && decision.status_code == 1008,
                 "WebSocket sender mismatch was not rejected");

    context.has_authenticated_node = false;
    push.sender = node_a;
    context.binary_message =
        mdbxc::sync::TransportMessageCodec::encode_push_request(push);
    decision = policy.check_websocket_message(context);
    require_true(!decision.allowed && decision.status_code == 1008,
                 "missing WebSocket authenticated node was not rejected");
}

void test_websocket_authenticated_node_policy_rejects_invalid_body() {
    const mdbxc::sync::NodeId node = make_node(0x33);
    const mdbxc::sync::DbId db_id = make_node(0xD3);

    mdbxc::sync::CodecBounds bounds;
    bounds.max_transport_message_bytes = 8;
    mdbxc::sync::WebSocketAuthenticatedNodeIdentityPolicy policy(bounds);

    mdbxc::sync::WebSocketSyncRequestContext context;
    context.has_authenticated_node = true;
    context.authenticated_node = node;
    context.allowed_dbs.insert(db_id);

    const std::uint8_t malformed_byte_1 = 0x01;
    const std::uint8_t malformed_byte_2 = 0x02;
    context.binary_message.push_back(malformed_byte_1);
    context.binary_message.push_back(malformed_byte_2);

    mdbxc::sync::SyncTransportDecision decision =
        policy.check_websocket_message(context);
    require_true(!decision.allowed && decision.status_code == 1007,
                 "malformed WebSocket sync body was not rejected as 1007");

    mdbxc::sync::PullRequest pull;
    pull.requester = node;
    pull.db_id = db_id;
    context.binary_message =
        mdbxc::sync::TransportMessageCodec::encode_pull_request(pull);

    decision = policy.check_websocket_message(context);
    require_true(!decision.allowed && decision.status_code == 1009,
                 "oversized WebSocket sync body was not rejected as 1009");
}

void test_websocket_server_middleware_rejects_spoofed_identity() {
    const std::string path = "test_websocket_transport_policy.mdbx";
    cleanup(path);

    std::shared_ptr<mdbxc::Connection> db = open_db(path);
    mdbxc::sync::SyncEngine engine(db);
    engine.initialize_local_identity(make_node(0x44), make_node(0xD4));

    mdbxc::sync::WebSocketSyncServer server(engine);
    mdbxc::sync::WebSocketAuthenticatedNodeIdentityPolicy policy;
    mdbxc::sync::WebSocketSyncServerMiddleware wrapped(server, &policy);

    const mdbxc::sync::NodeId authenticated = make_node(0x55);
    mdbxc::sync::PullRequest pull;
    pull.requester = make_node(0x66);
    pull.db_id = make_node(0xD4);

    mdbxc::sync::WebSocketSyncRequestContext context;
    context.has_authenticated_node = true;
    context.authenticated_node = authenticated;
    context.binary_message =
        mdbxc::sync::TransportMessageCodec::encode_pull_request(pull);

    bool caught = false;
    try {
        (void)wrapped.handle_binary_message(context);
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()).find("requester does not match") !=
                 std::string::npos;
    }

    db->disconnect();
    cleanup(path);

    require_true(caught,
                 "WebSocket server middleware allowed spoofed requester");
}

} // namespace

int main() {
    test_websocket_peer_pull_and_push_roundtrip();
    test_websocket_server_rejects_response_messages();
    test_websocket_server_rejects_malformed_messages();
    test_websocket_authenticated_node_policy();
    test_websocket_authenticated_node_policy_rejects_invalid_body();
    test_websocket_server_middleware_rejects_spoofed_identity();
    return 0;
}
