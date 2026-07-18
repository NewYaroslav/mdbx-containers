/**
 * \ingroup mdbxc_examples
 * \brief WebSocket binding over Simple-WebSocket-Server and standalone Asio.
 *
 * This example binds the framework-neutral WebSocketSyncServer /
 * WebSocketSyncPeer seam to eidheim/Simple-WebSocket-Server with
 * chriskohlhoff standalone Asio. It sends complete binary WebSocket messages
 * encoded by TransportMessageCodec.
 *
 * Build it explicitly:
 *
 *   cmake -S . -B tmp/build-ws-example \
 *       -DMDBXC_DEPS_MODE=BUNDLED \
 *       -DMDBXC_BUILD_TESTS=OFF \
 *       -DMDBXC_BUILD_EXAMPLES=ON \
 *       -DMDBXC_WEBSOCKET_SYNC_EXAMPLE=ON \
 *       -DCMAKE_CXX_STANDARD=11
 *   cmake --build tmp/build-ws-example \
 *       --target sync_17_websocket_simple_web_server
 *
 * Run the self-contained demo:
 *
 *   ./tmp/build-ws-example/bin/examples/sync_17_websocket_simple_web_server
 *
 * Expected output:
 *
 *   [websocket server] listening on 127.0.0.1:18081
 *   [websocket client] pull applied 2 batch(es)
 *   [websocket client] push sent 1 batch(es)
 *   [websocket client] request_cancel forwarded once
 *   OK: sync_17_websocket_simple_web_server
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE 1
#endif
#ifndef USE_STANDALONE_ASIO
#define USE_STANDALONE_ASIO 1
#endif
#include <client_ws.hpp>
#include <server_ws.hpp>

#include "sync_example_utils.hpp"

namespace {

using WsClient = SimpleWeb::SocketClient<SimpleWeb::WS>;
using WsServer = SimpleWeb::SocketServer<SimpleWeb::WS>;

const std::uint8_t kPrimaryNodeSeed = 0xC8;
const std::uint8_t kReplicaNodeSeed = 0xC9;
const std::uint8_t kDatabaseSeed = 0xCA;
const unsigned char kBinaryFrameOpcode = 130;

std::string bytes_to_string(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return std::string();
    }
    return std::string(reinterpret_cast<const char*>(&bytes[0]),
                       bytes.size());
}

std::vector<std::uint8_t> string_to_bytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string endpoint(const std::string& host, std::uint16_t port) {
    return host + ":" + std::to_string(static_cast<unsigned>(port));
}

class ExchangeState {
public:
    void set_value(const std::vector<std::uint8_t>& bytes) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_completed) {
            m_completed = true;
            m_promise.set_value(bytes);
        }
    }

    void set_exception(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_completed) {
            m_completed = true;
            m_promise.set_exception(std::make_exception_ptr(
                std::runtime_error(message)));
        }
    }

    std::future<std::vector<std::uint8_t> > future() {
        return m_promise.get_future();
    }

private:
    std::mutex m_mutex;
    bool m_completed = false;
    std::promise<std::vector<std::uint8_t> > m_promise;
};

class SimpleWebSocketSyncChannel
    : public mdbxc::sync::IWebSocketSyncChannel {
public:
    SimpleWebSocketSyncChannel(const std::string& host,
                               std::uint16_t port,
                               const std::string& bearer_token)
        : m_endpoint(endpoint(host, port) + "/mdbxc/sync/v1/ws"),
          m_bearer_token(bearer_token),
          m_cancel_generation(0),
          m_active_client(nullptr),
          m_cancel_count(0) {}

    std::vector<std::uint8_t> exchange_binary(
            const std::vector<std::uint8_t>& binary_message,
            const mdbxc::sync::CancellationToken& cancel_token) override {
        if (cancel_token.is_cancellation_requested()) {
            throw std::runtime_error("cancelled before WebSocket exchange");
        }

        const std::uint64_t call_generation =
            m_cancel_generation.load(std::memory_order_acquire);

        WsClient client(m_endpoint);
        client.config.header.emplace(
            "Authorization", std::string("Bearer ") + m_bearer_token);
        ActiveClientGuard active(*this, client);

        std::shared_ptr<ExchangeState> state(new ExchangeState());
        std::future<std::vector<std::uint8_t> > result = state->future();
        const std::string outbound = bytes_to_string(binary_message);

        client.on_open =
            [state, outbound](
                std::shared_ptr<WsClient::Connection> connection) {
                try {
                    connection->send(outbound, nullptr, kBinaryFrameOpcode);
                } catch (const std::exception& e) {
                    state->set_exception(e.what());
                    connection->send_close(1011);
                }
            };

        client.on_message =
            [state](
                std::shared_ptr<WsClient::Connection> connection,
                std::shared_ptr<WsClient::InMessage> in_message) {
                state->set_value(string_to_bytes(in_message->string()));
                connection->send_close(1000);
            };

        client.on_close =
            [state](
                std::shared_ptr<WsClient::Connection> connection,
                int status,
                const std::string& reason) {
                (void)connection;
                if (status != 1000) {
                    state->set_exception(
                        "WebSocket closed with status " +
                        std::to_string(status) + ": " + reason);
                }
            };

        client.on_error =
            [state](
                std::shared_ptr<WsClient::Connection> connection,
                const SimpleWeb::error_code& ec) {
                (void)connection;
                state->set_exception(ec.message());
            };

        if (cancel_token.is_cancellation_requested() ||
            m_cancel_generation.load(std::memory_order_acquire) !=
                call_generation) {
            throw std::runtime_error("cancelled before WebSocket start");
        }

        client.start();
        return result.get();
    }

    void request_cancel() override {
        m_cancel_generation.fetch_add(1, std::memory_order_acq_rel);
        m_cancel_count.fetch_add(1, std::memory_order_acq_rel);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active_client != nullptr) {
            m_active_client->stop();
        }
    }

    std::size_t cancel_count() const {
        return m_cancel_count.load(std::memory_order_acquire);
    }

private:
    class ActiveClientGuard {
    public:
        ActiveClientGuard(SimpleWebSocketSyncChannel& owner,
                          WsClient& client)
            : m_owner(owner), m_client(&client) {
            m_owner.set_active_client(m_client);
        }

        ~ActiveClientGuard() {
            m_owner.clear_active_client(m_client);
        }

        ActiveClientGuard(const ActiveClientGuard&) = delete;
        ActiveClientGuard& operator=(const ActiveClientGuard&) = delete;

    private:
        SimpleWebSocketSyncChannel& m_owner;
        WsClient* m_client;
    };

    void set_active_client(WsClient* client) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active_client = client;
    }

    void clear_active_client(WsClient* client) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active_client == client) {
            m_active_client = nullptr;
        }
    }

    std::string m_endpoint;
    std::string m_bearer_token;
    std::atomic<std::uint64_t> m_cancel_generation;
    mutable std::mutex m_mutex;
    WsClient* m_active_client;
    std::atomic<std::size_t> m_cancel_count;
};

class SimpleWebSocketSyncListener {
public:
    SimpleWebSocketSyncListener(
            mdbxc::sync::WebSocketSyncServerMiddleware& server,
            const std::string& bearer_token,
            const mdbxc::sync::NodeId& authenticated_node,
            const mdbxc::sync::DbId& allowed_db,
            const std::string& host,
            std::uint16_t port)
        : m_server(server),
          m_bearer_token(bearer_token),
          m_authenticated_node(authenticated_node),
          m_allowed_db(allowed_db),
          m_host(host),
          m_port(port),
          m_running(false) {
        m_ws.config.address = host;
        m_ws.config.port = port;
        m_ws.config.thread_pool_size = 1;

        WsServer::Endpoint& endpoint =
            m_ws.endpoint["^/mdbxc/sync/v1/ws/?$"];

        endpoint.on_handshake =
            [this](
                std::shared_ptr<WsServer::Connection> connection,
                SimpleWeb::CaseInsensitiveMultimap& response_header) {
                (void)response_header;
                const SimpleWeb::CaseInsensitiveMultimap::const_iterator it =
                    connection->header.find("Authorization");
                if (it == connection->header.end() ||
                    it->second !=
                        std::string("Bearer ") + m_bearer_token) {
                    return SimpleWeb::StatusCode::client_error_unauthorized;
                }
                return SimpleWeb::StatusCode::information_switching_protocols;
            };

        endpoint.on_message =
            [this](
                std::shared_ptr<WsServer::Connection> connection,
                std::shared_ptr<WsServer::InMessage> in_message) {
                try {
                    mdbxc::sync::WebSocketSyncRequestContext context;
                    context.has_authenticated_node = true;
                    context.authenticated_node = m_authenticated_node;
                    context.allowed_dbs.insert(m_allowed_db);
                    context.binary_message =
                        string_to_bytes(in_message->string());
                    const std::vector<std::uint8_t> response =
                        m_server.handle_binary_message(context);
                    connection->send(bytes_to_string(response),
                                     nullptr,
                                     kBinaryFrameOpcode);
                } catch (const std::exception& e) {
                    connection->send_close(1008, e.what());
                }
            };

        endpoint.on_error =
            [](
                std::shared_ptr<WsServer::Connection> connection,
                const SimpleWeb::error_code& ec) {
                (void)connection;
                (void)ec;
            };
    }

    ~SimpleWebSocketSyncListener() {
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
                bool started_callback_called = false;
                try {
                    m_ws.start(
                        [started, &started_callback_called](
                            unsigned short assigned_port) {
                            started_callback_called = true;
                            started->set_value(assigned_port);
                        });
                } catch (...) {
                    if (!started_callback_called) {
                        try {
                            started->set_exception(std::current_exception());
                        } catch (...) {}
                    }
                }
            });

        try {
            m_port = started_future.get();
        } catch (...) {
            if (m_thread.joinable()) {
                m_thread.join();
            }
            throw;
        }
        m_running = true;
        std::printf("[websocket server] listening on %s:%u\n",
                    m_host.c_str(),
                    static_cast<unsigned>(m_port));
    }

    void stop() {
        if (!m_running) {
            return;
        }
        m_ws.stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_running = false;
    }

private:
    mdbxc::sync::WebSocketSyncServerMiddleware& m_server;
    std::string m_bearer_token;
    mdbxc::sync::NodeId m_authenticated_node;
    mdbxc::sync::DbId m_allowed_db;
    std::string m_host;
    std::uint16_t m_port;
    WsServer m_ws;
    std::thread m_thread;
    bool m_running;
};

void seed_primary_rows(const std::shared_ptr<mdbxc::Connection>& db) {
    mdbxc::sync::ThreadLocalChangeAccumulator sink(db);
    mdbxc::KeyValueTable<int, std::string> quotes(db, "quotes");
    db->attach_sync_capture(&sink);
    quotes.insert_or_assign(1, "BTC/USD");
    quotes.insert_or_assign(2, "ETH/USD");
    db->detach_sync_capture();
}

void seed_replica_rows(const std::shared_ptr<mdbxc::Connection>& db) {
    mdbxc::sync::ThreadLocalChangeAccumulator sink(db);
    mdbxc::KeyValueTable<int, std::string> quotes(db, "quotes");
    db->attach_sync_capture(&sink);
    quotes.insert_or_assign(3, "SOL/USD");
    db->detach_sync_capture();
}

void require_quote(const std::shared_ptr<mdbxc::Connection>& db,
                   int key,
                   const std::string& expected) {
    mdbxc::KeyValueTable<int, std::string> quotes(db, "quotes");
    const std::string actual =
        sync_example::kv_or_throw(db, quotes, key, "quotes");
    sync_example::require(actual == expected,
                          "quote mismatch for key " +
                          std::to_string(key));
}

} // namespace

int main() {
    const std::string primary_path = "sync_17_primary.mdbx";
    const std::string replica_path = "sync_17_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const std::string host = "127.0.0.1";
    const std::uint16_t port = 18081;
    const std::string bearer_token = "replica-token";

    try {
        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica =
            sync_example::open(replica_path);

        const mdbxc::sync::NodeId primary_node =
            sync_example::make_node(kPrimaryNodeSeed);
        const mdbxc::sync::NodeId replica_node =
            sync_example::make_node(kReplicaNodeSeed);
        const mdbxc::sync::DbId db_id =
            sync_example::make_node(kDatabaseSeed);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_engine(replica);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        seed_primary_rows(primary);

        mdbxc::sync::WebSocketSyncServer ws_server(primary_engine);
        mdbxc::sync::WebSocketAuthenticatedNodeIdentityPolicy ws_identity;
        mdbxc::sync::WebSocketSyncServerMiddleware ws_middleware(
            ws_server, &ws_identity);
        SimpleWebSocketSyncListener listener(
            ws_middleware, bearer_token, replica_node, db_id, host, port);
        listener.start();

        SimpleWebSocketSyncChannel channel(host, port, bearer_token);
        mdbxc::sync::WebSocketSyncPeer peer(channel);

        mdbxc::sync::PullRequest pull;
        pull.requester = replica_node;
        pull.db_id = db_id;
        pull.have = replica_engine.applied_cursor();
        pull.max_batches = 100;

        const mdbxc::sync::PullResponse pulled = peer.pull(pull);
        sync_example::require(pulled.ok,
                              "WebSocket pull failed: " + pulled.error);

        mdbxc::sync::PushRequest local_apply;
        local_apply.sender = primary_node;
        local_apply.db_id = db_id;
        local_apply.batches = pulled.batches;
        const mdbxc::sync::PushResponse applied =
            replica_engine.handle_push(local_apply);
        sync_example::require(applied.ok,
                              "local apply failed: " + applied.error);
        std::printf("[websocket client] pull applied %zu batch(es)\n",
                    pulled.batches.size());

        seed_replica_rows(replica);
        const mdbxc::sync::PushRequest push =
            replica_engine.make_push_request(1, 0);
        const mdbxc::sync::PushResponse pushed = peer.push(push);
        sync_example::require(pushed.ok,
                              "WebSocket push failed: " + pushed.error);
        std::printf("[websocket client] push sent %zu batch(es)\n",
                    push.batches.size());

        peer.request_cancel();
        sync_example::require(channel.cancel_count() == 1u,
                              "request_cancel was not forwarded");
        std::printf("[websocket client] request_cancel forwarded once\n");

        require_quote(replica, 1, "BTC/USD");
        require_quote(replica, 2, "ETH/USD");
        require_quote(primary, 3, "SOL/USD");

        listener.stop();
        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_17_websocket_simple_web_server\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
