#include <mdbx_containers/sync.hpp>

#include "test_assert.hpp"

#include <cstdint>
#include <string>
#include <vector>

#ifndef MDBXC_SYNC_ENABLED
#error "MDBXC_SYNC_ENABLED must be defined by the test target"
#endif

#if !MDBXC_SYNC_ENABLED
#error "header_sync_umbrella_test must be compiled with sync enabled"
#endif

int main() {
    mdbxc::sync::NodeId node = mdbxc::sync::make_zero_node();
    MDBXC_TEST_ASSERT(node.size() == 16u);

    mdbxc::sync::ChangeOp op;
    op.op_type = mdbxc::sync::ChangeOpType::Put;
    op.dbi_name = "sync_umbrella_table";
    op.storage_key.push_back(1u);
    op.value.push_back(2u);

    mdbxc::sync::ChangeBatch batch;
    batch.origin_node_id = node;
    batch.seq = 1;
    batch.ops.push_back(op);

    const std::vector<std::uint8_t> encoded =
        mdbxc::sync::ChangeBatchCodec::encode(batch);
    const mdbxc::sync::ChangeBatch decoded =
        mdbxc::sync::ChangeBatchCodec::decode_exact(encoded);
    MDBXC_TEST_ASSERT(decoded.seq == 1u);
    MDBXC_TEST_ASSERT(decoded.ops.size() == 1u);
    MDBXC_TEST_ASSERT(decoded.ops[0].dbi_name == "sync_umbrella_table");

    mdbxc::sync::PullRequest pull;
    pull.requester = node;
    pull.db_id = node;
    pull.have.last_seq_by_origin[node] = 1u;
    const std::vector<std::uint8_t> wire =
        mdbxc::sync::TransportMessageCodec::encode_pull_request(pull);
    const mdbxc::sync::PullRequest decoded_pull =
        mdbxc::sync::TransportMessageCodec::decode_pull_request(wire);
    MDBXC_TEST_ASSERT(decoded_pull.requester == node);
    MDBXC_TEST_ASSERT(decoded_pull.have.last_seq_for(node) == 1u);
    MDBXC_TEST_ASSERT(
        mdbxc::sync::TransportMessageCodec::peek_message_type(wire) ==
        mdbxc::sync::TransportMessageType::PullRequest);
    MDBXC_TEST_ASSERT(std::string(mdbxc::sync::HttpSyncRoutes::pull_target())
                      == "/mdbxc/sync/v1/pull");
    mdbxc::sync::IWebSocketSyncChannel* websocket_channel = nullptr;
    mdbxc::sync::WebSocketSyncServer* websocket_server = nullptr;
    (void)websocket_channel;
    (void)websocket_server;
    mdbxc::sync::WebSocketSyncRequestContext websocket_context;
    websocket_context.has_authenticated_node = true;
    websocket_context.authenticated_node = node;
    websocket_context.binary_message = wire;
    mdbxc::sync::WebSocketAuthenticatedNodeIdentityPolicy websocket_policy;
    MDBXC_TEST_ASSERT(
        websocket_policy.check_websocket_message(websocket_context).allowed);
    mdbxc::sync::WebSocketSyncRejected websocket_rejection(
        1008u, "sync websocket rejected");
    MDBXC_TEST_ASSERT(websocket_rejection.close_code() == 1008u);
    mdbxc::sync::NodeDbAllowListPolicy allow_list;
    allow_list.allow_node_id(node);
    allow_list.allow_db_id(node);
    MDBXC_TEST_ASSERT(allow_list.check_pull(pull).allowed);

    mdbxc::sync::CancellationSource source;
    const mdbxc::sync::CancellationToken token = source.token();
    MDBXC_TEST_ASSERT(token.can_be_cancelled());
    source.request_cancel();
    MDBXC_TEST_ASSERT(token.is_cancellation_requested());

    return 0;
}
