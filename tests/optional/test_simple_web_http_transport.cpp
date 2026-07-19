#include <mdbx_containers/sync/transports/simple_web/HttpTransport.hpp>

#include "../test_assert.hpp"

#if !defined(MDBXC_HAS_SIMPLE_WEB_HTTP_TRANSPORT)
#error "Simple-Web HTTP transport target must define its feature macro"
#endif

#if !MDBXC_HAS_SIMPLE_WEB_HTTP_TRANSPORT
#error "Simple-Web HTTP transport feature macro must be non-zero"
#endif

int main() {
    mdbxc::sync::simple_web::HttpSyncClientConfig config;
    config.host = "127.0.0.1";
    config.port = 18080;

    MDBXC_TEST_ASSERT(config.host == "127.0.0.1");
    MDBXC_TEST_ASSERT(config.port == 18080u);

    return 0;
}
