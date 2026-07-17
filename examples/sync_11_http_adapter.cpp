/**
 * \ingroup mdbxc_examples
 * \brief HTTP-shaped sync adapter without choosing an HTTP framework.
 *
 * `HttpSyncPeer` implements `ISyncPeer` by sending binary request bodies to an
 * `IHttpSyncClient`. `HttpSyncServer` handles the matching server-side route
 * after a real HTTP framework has parsed method, target, content type, and
 * body.
 *
 * This example uses an in-process loopback client so the code stays portable.
 * Replace `LoopbackHttpClient::post()` with libcurl, Boost.Beast,
 * Simple-Web-Server, cpp-httplib, or another framework, and keep the same
 * route/body contract.
 *
 * Expected output:
 *   [http adapter] pull delivered 2 batch(es)
 *   [http adapter] push delivered 1 batch(es)
 *   OK: sync_11_http_adapter
 */

#include "sync_example_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

class LoopbackHttpClient : public mdbxc::sync::IHttpSyncClient {
public:
    explicit LoopbackHttpClient(mdbxc::sync::HttpSyncServer& server)
        : m_server(server), m_cancel_count(0) {}

    mdbxc::sync::HttpSyncResponse post(
            const std::string& target,
            const std::string& content_type,
            const std::vector<std::uint8_t>& body,
            const mdbxc::sync::CancellationToken& cancel_token) override {
        // A real HTTP client would put `content_type` into the Content-Type
        // header, write `body` as the request body, and map `cancel_token` plus
        // request_cancel() to its timeout/socket cancellation primitive.
        (void)cancel_token;

        mdbxc::sync::HttpSyncRequest request;
        request.method = mdbxc::sync::HttpSyncRoutes::method_post();
        request.target = target;
        request.content_type = content_type;
        request.body = body;
        return m_server.handle(request);
    }

    void request_cancel() override {
        // A real implementation would interrupt the active HTTP request here.
        ++m_cancel_count;
    }

    std::size_t cancel_count() const { return m_cancel_count; }

private:
    mdbxc::sync::HttpSyncServer& m_server;
    std::size_t m_cancel_count;
};

} // namespace

int main() {
    const std::string primary_path = "sync_11_primary.mdbx";
    const std::string replica_path = "sync_11_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(0xE0);
    const mdbxc::sync::NodeId replica_node =
        sync_example::make_node(0xE1);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(0xE2);

    try {
        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica =
            sync_example::open(replica_path);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_engine(replica);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        mdbxc::sync::ThreadLocalChangeAccumulator capture(primary);
        mdbxc::KeyValueTable<int, std::string> primary_ticks(primary, "ticks");

        primary->attach_sync_capture(&capture);
        primary_ticks.insert_or_assign(1, "BTC/USD");
        primary_ticks.insert_or_assign(2, "ETH/USD");
        primary->detach_sync_capture();

        mdbxc::sync::HttpSyncServer primary_server(primary_engine);
        LoopbackHttpClient primary_http(primary_server);
        mdbxc::sync::HttpSyncPeer primary_peer(primary_http);

        mdbxc::sync::PullRequest pull;
        pull.requester = replica_node;
        pull.db_id = db_id;
        pull.have = replica_engine.applied_cursor();
        pull.max_batches = 100;

        const mdbxc::sync::PullResponse pulled = primary_peer.pull(pull);
        sync_example::require(pulled.ok,
                              "HTTP pull failed: " + pulled.error);

        mdbxc::sync::PushRequest local_apply;
        local_apply.sender = primary_node;
        local_apply.db_id = db_id;
        local_apply.batches = pulled.batches;
        const mdbxc::sync::PushResponse applied =
            replica_engine.handle_push(local_apply);
        sync_example::require(applied.ok,
                              "local apply failed: " + applied.error);
        std::printf("[http adapter] pull delivered %zu batch(es)\n",
                    pulled.batches.size());

        primary->attach_sync_capture(&capture);
        primary_ticks.insert_or_assign(3, "SOL/USD");
        primary->detach_sync_capture();

        mdbxc::sync::HttpSyncServer replica_server(replica_engine);
        LoopbackHttpClient replica_http(replica_server);
        mdbxc::sync::HttpSyncPeer replica_peer(replica_http);

        const mdbxc::sync::PushRequest push =
            primary_engine.make_push_request(3, 0);
        const mdbxc::sync::PushResponse pushed = replica_peer.push(push);
        sync_example::require(pushed.ok,
                              "HTTP push failed: " + pushed.error);
        std::printf("[http adapter] push delivered %zu batch(es)\n",
                    push.batches.size());

        replica_peer.request_cancel();
        sync_example::require(replica_http.cancel_count() == 1u,
                              "request_cancel was not forwarded");

        mdbxc::KeyValueTable<int, std::string> replica_ticks(replica, "ticks");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 1,
                                      "ticks[1]") == "BTC/USD",
            "replica ticks[1] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 2,
                                      "ticks[2]") == "ETH/USD",
            "replica ticks[2] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 3,
                                      "ticks[3]") == "SOL/USD",
            "replica ticks[3] mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_11_http_adapter\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
