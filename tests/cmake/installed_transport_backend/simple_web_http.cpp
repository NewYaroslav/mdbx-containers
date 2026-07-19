#include <mdbx_containers/sync/transports/simple_web/HttpTransport.hpp>

#if !defined(MDBXC_SYNC_ENABLED) || !MDBXC_SYNC_ENABLED
#error "transport usage target must enable MDBXC_SYNC_ENABLED"
#endif

#if !defined(MDBXC_HAS_SIMPLE_WEB_HTTP_TRANSPORT) || \
        !MDBXC_HAS_SIMPLE_WEB_HTTP_TRANSPORT
#error "Simple-Web HTTP target must expose MDBXC_HAS_SIMPLE_WEB_HTTP_TRANSPORT"
#endif

int main() {
    mdbxc::sync::simple_web::HttpSyncListenerConfig config;
    return config.sync_route_regex.empty() ? 1 : 0;
}
