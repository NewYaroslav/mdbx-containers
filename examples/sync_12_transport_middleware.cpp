/**
 * \ingroup mdbxc_examples
 * \brief Transport middleware for allow-lists, rate limits, and metrics.
 *
 * This example stacks two framework-neutral middleware layers:
 *
 *   SyncPeerMiddleware
 *     -> HttpSyncPeer
 *        -> HttpSyncClientMiddleware
 *           -> application HTTP client
 *
 * `SyncPeerMiddleware` sees decoded DTOs and can inspect `NodeId` / `DbId`.
 * `HttpSyncClientMiddleware` sees HTTP-shaped target/content-type/body values
 * and can enforce route-level policy before a real HTTP library sends bytes.
 *
 * Expected output:
 *   [middleware] allowed pull applied 2 batch(es)
 *   [middleware] db policy rejected pull: sync db_id is not allowed
 *   [middleware] rate limit rejected pull: sync pull rate limit exceeded
 *   [middleware] HTTP route rejected with 403
 *   [middleware] metrics pull=1 http=1 rejected=3
 *   OK: sync_12_transport_middleware
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
        (void)cancel_token;
        mdbxc::sync::HttpSyncRequest request;
        request.method = mdbxc::sync::HttpSyncRoutes::method_post();
        request.target = target;
        request.content_type = content_type;
        request.body = body;
        return m_server.handle(request);
    }

    void request_cancel() override {
        ++m_cancel_count;
    }

    std::size_t cancel_count() const { return m_cancel_count; }

private:
    mdbxc::sync::HttpSyncServer& m_server;
    std::size_t m_cancel_count;
};

} // namespace

int main() {
    const std::string primary_path = "sync_12_primary.mdbx";
    const std::string replica_path = "sync_12_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(0xC0);
    const mdbxc::sync::NodeId replica_node =
        sync_example::make_node(0xC1);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(0xC2);
    const mdbxc::sync::DbId denied_db_id =
        sync_example::make_node(0xC3);

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

        mdbxc::sync::SyncTransportMetricsObserver metrics;

        mdbxc::sync::HttpRouteAllowListPolicy routes;
        routes.allow_target(mdbxc::sync::HttpSyncRoutes::pull_target());

        mdbxc::sync::HttpSyncServer primary_server(primary_engine);
        LoopbackHttpClient raw_http(primary_server);
        mdbxc::sync::HttpSyncClientMiddleware guarded_http(
            raw_http, &routes, &metrics);
        mdbxc::sync::HttpSyncPeer http_peer(guarded_http);

        mdbxc::sync::NodeDbAllowListPolicy allow_list;
        allow_list.allow_node_id(replica_node);
        allow_list.allow_db_id(db_id);

        mdbxc::sync::FixedBudgetSyncTransportPolicy budget(1, 100);
        mdbxc::sync::CompositeSyncTransportPolicy peer_policy;
        peer_policy.add(allow_list);
        peer_policy.add(budget);

        mdbxc::sync::SyncPeerMiddleware guarded_peer(
            http_peer, &peer_policy, &metrics);

        mdbxc::sync::PullRequest pull;
        pull.requester = replica_node;
        pull.db_id = db_id;
        pull.have = replica_engine.applied_cursor();
        pull.max_batches = 100;

        const mdbxc::sync::PullResponse pulled = guarded_peer.pull(pull);
        sync_example::require(pulled.ok,
                              "allowed pull failed: " + pulled.error);

        mdbxc::sync::PushRequest local_apply;
        local_apply.sender = primary_node;
        local_apply.db_id = db_id;
        local_apply.batches = pulled.batches;
        const mdbxc::sync::PushResponse applied =
            replica_engine.handle_push(local_apply);
        sync_example::require(applied.ok,
                              "local apply failed: " + applied.error);
        std::printf("[middleware] allowed pull applied %zu batch(es)\n",
                    pulled.batches.size());

        pull.db_id = denied_db_id;
        const mdbxc::sync::PullResponse denied_by_db =
            guarded_peer.pull(pull);
        sync_example::require(!denied_by_db.ok,
                              "denied db_id was allowed");
        std::printf("[middleware] db policy rejected pull: %s\n",
                    denied_by_db.error.c_str());

        pull.db_id = db_id;
        const mdbxc::sync::PullResponse denied_by_budget =
            guarded_peer.pull(pull);
        sync_example::require(!denied_by_budget.ok,
                              "rate-limited pull was allowed");
        std::printf("[middleware] rate limit rejected pull: %s\n",
                    denied_by_budget.error.c_str());

        const mdbxc::sync::HttpSyncResponse denied_route =
            guarded_http.post(mdbxc::sync::HttpSyncRoutes::push_target(),
                              mdbxc::sync::HttpSyncRoutes::content_type(),
                              std::vector<std::uint8_t>(),
                              mdbxc::sync::CancellationToken());
        sync_example::require(denied_route.status_code == 403,
                              "denied HTTP route was allowed");
        std::printf("[middleware] HTTP route rejected with %u\n",
                    denied_route.status_code);

        guarded_peer.request_cancel();
        sync_example::require(raw_http.cancel_count() == 1u,
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

        const mdbxc::sync::SyncTransportMetricsSnapshot snapshot =
            metrics.snapshot();
        sync_example::require(snapshot.pull_calls == 1u,
                              "pull metric mismatch");
        sync_example::require(snapshot.http_post_calls == 1u,
                              "HTTP post metric mismatch");
        sync_example::require(snapshot.rejected_calls == 3u,
                              "rejection metric mismatch");
        std::printf("[middleware] metrics pull=%llu http=%llu rejected=%llu\n",
                    static_cast<unsigned long long>(snapshot.pull_calls),
                    static_cast<unsigned long long>(snapshot.http_post_calls),
                    static_cast<unsigned long long>(snapshot.rejected_calls));

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_12_transport_middleware\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
