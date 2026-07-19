#include <mdbx_containers/sync/transport.hpp>

#include "test_assert.hpp"

#include <cstdint>
#include <string>
#include <vector>

#ifndef MDBXC_SYNC_ENABLED
#error "MDBXC_SYNC_ENABLED must be defined by the test target"
#endif

#if !MDBXC_SYNC_ENABLED
#error "header_sync_transport_umbrella_test must be compiled with sync enabled"
#endif

#if defined(MDBXC_HAS_SIMPLE_WEB_HTTP_TRANSPORT)
#error "framework-neutral sync/transport.hpp must not enable Simple-Web HTTP"
#endif

#if defined(MDBXC_HAS_SIMPLE_WEB_WEBSOCKET_TRANSPORT)
#error "framework-neutral sync/transport.hpp must not enable Simple-WebSocket"
#endif

#if defined(MDBXC_HAS_KURLYK_HTTP_TRANSPORT)
#error "framework-neutral sync/transport.hpp must not enable Kurlyk HTTP"
#endif

int main() {
    mdbxc::sync::NodeId node = mdbxc::sync::make_zero_node();
    mdbxc::sync::PullRequest pull;
    pull.requester = node;
    pull.db_id = node;
    pull.max_batches = 1;

    const std::vector<std::uint8_t> encoded =
        mdbxc::sync::TransportMessageCodec::encode_pull_request(pull);
    MDBXC_TEST_ASSERT(
        mdbxc::sync::TransportMessageCodec::peek_message_type(encoded) ==
        mdbxc::sync::TransportMessageType::PullRequest);

    MDBXC_TEST_ASSERT(std::string(mdbxc::sync::HttpSyncRoutes::pull_target())
                      == "/mdbxc/sync/v1/pull");
    MDBXC_TEST_ASSERT(mdbxc::sync::http_sync_status_is_retryable(503u));
    MDBXC_TEST_ASSERT(
        mdbxc::sync::websocket_sync_close_code_is_retryable(1013u));

    mdbxc::sync::TransportMessageSizePolicy size_policy(1024u);
    MDBXC_TEST_ASSERT(size_policy.check_pull(pull).allowed);

    mdbxc::sync::WebSocketSyncRequestContext websocket_context;
    websocket_context.has_authenticated_node = true;
    websocket_context.authenticated_node = node;
    websocket_context.db_access = mdbxc::sync::SyncDbAccess::only(node);
    websocket_context.binary_message = encoded;

    mdbxc::sync::WebSocketAuthenticatedNodeIdentityPolicy websocket_policy;
    MDBXC_TEST_ASSERT(
        websocket_policy.check_websocket_message(websocket_context).allowed);

    return 0;
}
