/**
 * \ingroup mdbxc_examples
 * \brief HTTP request-context policies around the sync adapter seam.
 *
 * This example shows the adapter-local policy layer that a concrete HTTP
 * server would run before `HttpSyncServer::handle()`: bearer token extraction,
 * remote-address allow-listing, and rate-limit rejections with `Retry-After`.
 *
 * Expected output:
 *   [http policy] authorized pull delivered 1 batch(es)
 *   [http policy] second pull rejected with Retry-After=...
 *   OK: sync_15_http_policy_context
 */

#include "sync_example_utils.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class ContextHttpClient : public mdbxc::sync::IHttpSyncClient {
public:
    ContextHttpClient(mdbxc::sync::HttpSyncServerMiddleware& server,
                      const std::string& bearer_token,
                      const std::string& remote_address)
        : m_server(server),
          m_bearer_token(bearer_token),
          m_remote_address(remote_address) {}

    mdbxc::sync::HttpSyncResponse post(
            const std::string& target,
            const std::string& content_type,
            const std::vector<std::uint8_t>& body,
            const mdbxc::sync::CancellationToken& cancel_token) override {
        (void)cancel_token;

        mdbxc::sync::HttpSyncRequest request;
        request.method = mdbxc::sync::HttpSyncRoutes::method_post();
        request.target = target;
        request.content_type = content_type;
        request.remote_address = m_remote_address;
        request.body = body;
        mdbxc::sync::http_add_header(
            request.headers, "Authorization",
            std::string("Bearer ") + m_bearer_token);

        m_last_response = m_server.handle(request);
        return m_last_response;
    }

    const mdbxc::sync::HttpSyncResponse& last_response() const {
        return m_last_response;
    }

private:
    mdbxc::sync::HttpSyncServerMiddleware& m_server;
    std::string m_bearer_token;
    std::string m_remote_address;
    mdbxc::sync::HttpSyncResponse m_last_response;
};

} // namespace

int main() {
    const std::string primary_path = "sync_15_primary.mdbx";
    const std::string replica_path = "sync_15_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(0xA4);
    const mdbxc::sync::NodeId replica_node =
        sync_example::make_node(0xA5);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(0xA6);

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
        primary->detach_sync_capture();

        mdbxc::sync::HttpRouteAllowListPolicy routes;
        routes.allow_target(mdbxc::sync::HttpSyncRoutes::pull_target());
        mdbxc::sync::HttpBearerTokenPolicy bearer;
        bearer.allow_token("demo-token");
        mdbxc::sync::HttpRemoteAddressAllowListPolicy remotes;
        remotes.allow_remote_address("127.0.0.1");
        mdbxc::sync::FixedWindowHttpRateLimitPolicy rate(
            1, std::chrono::seconds(5));

        mdbxc::sync::CompositeSyncTransportPolicy policy;
        policy.add(routes);
        policy.add(bearer);
        policy.add(remotes);
        policy.add(rate);

        mdbxc::sync::HttpSyncServer primary_server(primary_engine);
        mdbxc::sync::HttpSyncServerMiddleware guarded_server(
            primary_server, &policy);
        ContextHttpClient raw_client(
            guarded_server, "demo-token", "127.0.0.1");
        mdbxc::sync::HttpSyncPeer peer(raw_client);

        mdbxc::sync::PullRequest pull;
        pull.requester = replica_node;
        pull.db_id = db_id;
        pull.have = replica_engine.applied_cursor();
        pull.max_batches = 100;

        const mdbxc::sync::PullResponse pulled = peer.pull(pull);
        sync_example::require(pulled.ok,
                              "authorized pull failed: " + pulled.error);
        std::printf("[http policy] authorized pull delivered %zu batch(es)\n",
                    pulled.batches.size());

        bool rejected = false;
        try {
            (void)peer.pull(pull);
        } catch (const std::runtime_error& e) {
            (void)e;
            rejected = true;
        }
        sync_example::require(rejected,
                              "second pull should hit rate limit");

        const std::string retry_after =
            mdbxc::sync::http_header_value(
                raw_client.last_response().headers, "Retry-After");
        sync_example::require(!retry_after.empty(),
                              "rate limit must return Retry-After");
        std::printf("[http policy] second pull rejected with Retry-After=%s\n",
                    retry_after.c_str());

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_15_http_policy_context\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
