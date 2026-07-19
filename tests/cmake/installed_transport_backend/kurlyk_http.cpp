#include <mdbx_containers/sync/transports/kurlyk/HttpTransport.hpp>

#if !defined(MDBXC_SYNC_ENABLED) || !MDBXC_SYNC_ENABLED
#error "transport usage target must enable MDBXC_SYNC_ENABLED"
#endif

#if !defined(MDBXC_HAS_KURLYK_HTTP_TRANSPORT) || \
        !MDBXC_HAS_KURLYK_HTTP_TRANSPORT
#error "Kurlyk HTTP target must expose MDBXC_HAS_KURLYK_HTTP_TRANSPORT"
#endif

int main() {
    mdbxc::sync::kurlyk::HttpSyncClientConfig config;
    return config.base_url.empty() ? 1 : 0;
}
