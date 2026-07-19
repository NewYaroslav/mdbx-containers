#include <mdbx_containers/sync/transports/simple_web/WebSocketTransport.hpp>

#if !defined(MDBXC_SYNC_ENABLED) || !MDBXC_SYNC_ENABLED
#error "transport usage target must enable MDBXC_SYNC_ENABLED"
#endif

#if !defined(MDBXC_HAS_SIMPLE_WEB_WEBSOCKET_TRANSPORT) || \
        !MDBXC_HAS_SIMPLE_WEB_WEBSOCKET_TRANSPORT
#error "Simple-WebSocket target must expose MDBXC_HAS_SIMPLE_WEB_WEBSOCKET_TRANSPORT"
#endif

int main() {
    mdbxc::sync::simple_web::WebSocketSyncListenerConfig config;
    return config.endpoint_regex.empty() ? 1 : 0;
}
