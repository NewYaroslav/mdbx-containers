#include <mdbx_containers/sync/transports/kurlyk/HttpTransport.hpp>

#include "../test_assert.hpp"

#if !defined(MDBXC_HAS_KURLYK_HTTP_TRANSPORT)
#error "Kurlyk HTTP transport target must define its feature macro"
#endif

#if !MDBXC_HAS_KURLYK_HTTP_TRANSPORT
#error "Kurlyk HTTP transport feature macro must be non-zero"
#endif

int main() {
    mdbxc::sync::kurlyk::HttpSyncClientConfig config;
    config.base_url = "http://127.0.0.1:18080";

    MDBXC_TEST_ASSERT(config.base_url == "http://127.0.0.1:18080");

    return 0;
}
