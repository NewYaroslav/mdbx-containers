#include <mdbx_containers/sync/transports/simple_web/WebSocketTransport.hpp>

#if !defined(MDBXC_HAS_SIMPLE_WEB_WEBSOCKET_TRANSPORT)
#error "Simple-WebSocket transport target must define its feature macro"
#endif

#if !MDBXC_HAS_SIMPLE_WEB_WEBSOCKET_TRANSPORT
#error "Simple-WebSocket transport feature macro must be non-zero"
#endif

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId node{};
    for (int i = 0; i < 16; ++i) {
        node[i] = static_cast<std::uint8_t>(seed + i);
    }
    return node;
}

class SilentWebSocketServer {
public:
    SilentWebSocketServer()
        : m_received(false),
          m_running(false) {
        m_server.config.address = "127.0.0.1";
        m_server.config.port = 0;
        m_server.config.thread_pool_size = 1;

        Server::Endpoint& endpoint =
            m_server.endpoint["^/mdbxc/sync/v1/ws/?$"];
        endpoint.on_message =
            [this](
                std::shared_ptr<Server::Connection> connection,
                std::shared_ptr<Server::InMessage> in_message) {
                (void)connection;
                (void)in_message;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_received = true;
                }
                m_changed.notify_all();
                // Intentionally keep the connection open and never send a
                // response. The client-side cancellation path must unblock the
                // exchange.
            };
    }

    ~SilentWebSocketServer() {
        stop();
    }

    void start() {
        if (m_running) {
            return;
        }

        std::shared_ptr<std::promise<unsigned short> > started(
            new std::promise<unsigned short>());
        std::future<unsigned short> started_future = started->get_future();
        m_thread = std::thread(
            [this, started]() {
                bool callback_called = false;
                try {
                    m_server.start(
                        [started, &callback_called](
                            unsigned short assigned_port) {
                            callback_called = true;
                            started->set_value(assigned_port);
                        });
                } catch (...) {
                    if (!callback_called) {
                        try {
                            started->set_exception(std::current_exception());
                        } catch (...) {}
                    }
                }
            });
        m_port = started_future.get();
        m_running = true;
    }

    void stop() {
        if (!m_running) {
            return;
        }
        m_server.stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_running = false;
    }

    unsigned short port() const {
        return m_port;
    }

    bool wait_for_message(std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout, [this] { return m_received; });
    }

private:
    typedef SimpleWeb::SocketServer<SimpleWeb::WS> Server;

    Server m_server;
    std::thread m_thread;
    unsigned short m_port = 0;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    bool m_received;
    bool m_running;
};

void require_true(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void test_websocket_channel_cancel_unblocks_silent_exchange() {
    SilentWebSocketServer server;
    server.start();

    mdbxc::sync::simple_web::WebSocketSyncChannelConfig config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.bounds.max_transport_message_bytes = 1024u;
    require_true(config.bounds.max_transport_message_bytes == 1024u,
                 "WebSocket channel bounds field was not configurable");
    mdbxc::sync::simple_web::WebSocketSyncChannel channel(config);

    mdbxc::sync::PullRequest request;
    request.requester = make_node(0x10);
    request.db_id = make_node(0xD0);
    const std::vector<std::uint8_t> message =
        mdbxc::sync::TransportMessageCodec::encode_pull_request(request);

    std::promise<std::string> result_promise;
    std::future<std::string> result = result_promise.get_future();
    mdbxc::sync::CancellationSource cancel;
    std::thread client(
        [&channel, &message, &cancel, &result_promise]() {
            try {
                (void)channel.exchange_binary(message, cancel.token());
                result_promise.set_value("completed");
            } catch (const std::exception& e) {
                result_promise.set_value(e.what());
            } catch (...) {
                result_promise.set_value("unknown");
            }
        });

    const bool server_received =
        server.wait_for_message(std::chrono::seconds(5));
    if (!server_received) {
        cancel.request_cancel();
        channel.request_cancel();
        if (client.joinable()) {
            client.join();
        }
        server.stop();
    }
    require_true(server_received,
                 "WebSocket server did not receive test message");

    cancel.request_cancel();
    channel.request_cancel();

    const bool exchange_ready =
        result.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
    if (!exchange_ready) {
        cancel.request_cancel();
        channel.request_cancel();
    }
    if (client.joinable()) {
        client.join();
    }
    server.stop();

    require_true(exchange_ready,
                 "WebSocket exchange did not unblock after cancellation");

    require_true(channel.cancel_count() == 1u,
                 "WebSocket channel did not count request_cancel");
    require_true(result.get().find("cancelled") != std::string::npos,
                 "WebSocket cancelled exchange reported wrong result");
}

void test_websocket_channel_rejects_oversized_outbound_message() {
    mdbxc::sync::simple_web::WebSocketSyncChannelConfig config;
    config.host = "127.0.0.1";
    config.port = 1;
    config.bounds.max_transport_message_bytes = 4u;
    mdbxc::sync::simple_web::WebSocketSyncChannel channel(config);

    bool rejected = false;
    try {
        mdbxc::sync::CancellationToken cancel;
        std::vector<std::uint8_t> message(5u, 0x42u);
        (void)channel.exchange_binary(message, cancel);
    } catch (const std::length_error& e) {
        rejected =
            std::string(e.what()).find("max_transport_message_bytes") !=
            std::string::npos;
    }

    require_true(rejected,
                 "WebSocket channel accepted oversized outbound message");
}

void test_websocket_channel_deadline_unblocks_silent_exchange() {
    SilentWebSocketServer server;
    server.start();

    mdbxc::sync::simple_web::WebSocketSyncChannelConfig config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.bounds.max_transport_message_bytes = 1024u;
    config.exchange_timeout = std::chrono::milliseconds(50);
    mdbxc::sync::simple_web::WebSocketSyncChannel channel(config);

    mdbxc::sync::PullRequest request;
    request.requester = make_node(0x20);
    request.db_id = make_node(0xD0);
    const std::vector<std::uint8_t> message =
        mdbxc::sync::TransportMessageCodec::encode_pull_request(request);

    std::promise<std::string> result_promise;
    std::future<std::string> result = result_promise.get_future();
    mdbxc::sync::CancellationSource cancel;
    std::thread client(
        [&channel, &message, &cancel, &result_promise]() {
            try {
                (void)channel.exchange_binary(message, cancel.token());
                result_promise.set_value("completed");
            } catch (const std::exception& e) {
                result_promise.set_value(e.what());
            } catch (...) {
                result_promise.set_value("unknown");
            }
        });

    const bool exchange_ready =
        result.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
    if (!exchange_ready) {
        channel.request_cancel();
    }
    if (client.joinable()) {
        client.join();
    }
    server.stop();

    require_true(exchange_ready,
                 "WebSocket exchange did not unblock after deadline");
    require_true(result.get().find("deadline") != std::string::npos,
                 "WebSocket deadline exchange reported wrong result");
}

void test_websocket_channel_rejects_negative_deadline() {
    mdbxc::sync::simple_web::WebSocketSyncChannelConfig config;
    config.host = "127.0.0.1";
    config.port = 1;
    config.exchange_timeout = std::chrono::milliseconds(-1);
    mdbxc::sync::simple_web::WebSocketSyncChannel channel(config);

    bool rejected = false;
    try {
        mdbxc::sync::CancellationToken cancel;
        std::vector<std::uint8_t> message(1u, 0x42u);
        (void)channel.exchange_binary(message, cancel);
    } catch (const std::invalid_argument& e) {
        rejected =
            std::string(e.what()).find("exchange_timeout") !=
            std::string::npos;
    }

    require_true(rejected,
                 "WebSocket channel accepted a negative exchange deadline");
}

} // namespace

int main() {
    test_websocket_channel_cancel_unblocks_silent_exchange();
    test_websocket_channel_rejects_oversized_outbound_message();
    test_websocket_channel_deadline_unblocks_silent_exchange();
    test_websocket_channel_rejects_negative_deadline();
    return 0;
}
