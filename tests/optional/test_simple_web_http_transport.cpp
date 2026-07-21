#include <mdbx_containers/sync/transports/simple_web/HttpTransport.hpp>

#include "../test_assert.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>

#if !defined(MDBXC_HAS_SIMPLE_WEB_HTTP_TRANSPORT)
#error "Simple-Web HTTP transport target must define its feature macro"
#endif

#if !MDBXC_HAS_SIMPLE_WEB_HTTP_TRANSPORT
#error "Simple-Web HTTP transport feature macro must be non-zero"
#endif

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
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path) {
    mdbxc::Config config;
    config.pathname = path;
    config.no_subdir = true;
    config.max_dbs = 16;
    return mdbxc::Connection::create(config);
}

void test_http_listener_rejects_oversized_body() {
    const std::string path = "test_simple_web_http_oversized.mdbx";
    cleanup(path);

    std::shared_ptr<mdbxc::Connection> conn = open_env(path);
    mdbxc::sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0x10), make_node(0xD0));

    mdbxc::sync::HttpSyncServer server(engine);
    mdbxc::sync::simple_web::HttpSyncListenerConfig listener_config;
    listener_config.host = "127.0.0.1";
    listener_config.port = 0;
    listener_config.bounds.max_transport_message_bytes = 4;

    mdbxc::sync::simple_web::HttpSyncListener listener(
        server, listener_config);
    listener.start();

    SimpleWeb::Client<SimpleWeb::HTTP> client(
        std::string("127.0.0.1:") +
        std::to_string(static_cast<unsigned>(listener.port())));
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mdbxc::sync::HttpSyncRoutes::content_type());

    const std::shared_ptr<SimpleWeb::Client<SimpleWeb::HTTP>::Response>
        response = client.request(
            "POST",
            mdbxc::sync::HttpSyncRoutes::pull_target(),
            "12345",
            headers);

    listener.stop();

    MDBXC_TEST_ASSERT(std::atoi(response->status_code.c_str()) == 413);
    MDBXC_TEST_ASSERT(
        response->content.string().find("max_transport_message_bytes") !=
        std::string::npos);

    conn->disconnect();
    cleanup(path);
}

} // namespace

int main() {
    mdbxc::sync::simple_web::HttpSyncClientConfig config;
    config.host = "127.0.0.1";
    config.port = 18080;
    config.bounds.max_transport_message_bytes = 32;

    MDBXC_TEST_ASSERT(config.host == "127.0.0.1");
    MDBXC_TEST_ASSERT(config.port == 18080u);
    MDBXC_TEST_ASSERT(config.bounds.max_transport_message_bytes == 32u);

    mdbxc::sync::simple_web::HttpSyncListenerConfig listener_config;
    listener_config.bounds.max_transport_message_bytes = 64;
    MDBXC_TEST_ASSERT(
        listener_config.bounds.max_transport_message_bytes == 64u);

    test_http_listener_rejects_oversized_body();

    return 0;
}
