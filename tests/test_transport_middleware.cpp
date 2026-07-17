#include <mdbx_containers/sync.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId node{};
    for (int i = 0; i < 16; ++i) {
        node[i] = static_cast<std::uint8_t>(seed + i);
    }
    return node;
}

mdbxc::sync::ChangeBatch make_batch(std::uint8_t seed, std::uint64_t seq) {
    mdbxc::sync::ChangeBatch batch;
    batch.origin_node_id = make_node(seed);
    batch.seq = seq;
    return batch;
}

void require_true(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class RecordingPeer : public mdbxc::sync::ISyncPeer {
public:
    RecordingPeer() : m_pull_count(0), m_push_count(0), m_cancel_count(0) {}

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        ++m_pull_count;
        m_last_pull = request;
        mdbxc::sync::PullResponse response;
        response.batches.push_back(make_batch(0xA0, m_pull_count));
        return response;
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        ++m_push_count;
        m_last_push = request;
        mdbxc::sync::PushResponse response;
        response.receiver_have.last_seq_by_origin[request.sender] =
            request.batches.size();
        return response;
    }

    void request_cancel() override {
        ++m_cancel_count;
    }

    std::size_t pull_count() const { return m_pull_count; }
    std::size_t push_count() const { return m_push_count; }
    std::size_t cancel_count() const { return m_cancel_count; }

private:
    std::size_t m_pull_count;
    std::size_t m_push_count;
    std::size_t m_cancel_count;
    mdbxc::sync::PullRequest m_last_pull;
    mdbxc::sync::PushRequest m_last_push;
};

class RecordingHttpClient : public mdbxc::sync::IHttpSyncClient {
public:
    RecordingHttpClient() : m_post_count(0), m_cancel_count(0) {}

    mdbxc::sync::HttpSyncResponse post(
            const std::string& target,
            const std::string& content_type,
            const std::vector<std::uint8_t>& body,
            const mdbxc::sync::CancellationToken& cancel_token) override {
        (void)cancel_token;
        ++m_post_count;
        m_last_target = target;
        mdbxc::sync::HttpSyncResponse response;
        response.status_code = 200;
        response.content_type = content_type;
        response.body = body;
        return response;
    }

    void request_cancel() override {
        ++m_cancel_count;
    }

    std::size_t post_count() const { return m_post_count; }
    std::size_t cancel_count() const { return m_cancel_count; }
    const std::string& last_target() const { return m_last_target; }

private:
    std::size_t m_post_count;
    std::size_t m_cancel_count;
    std::string m_last_target;
};

class ThrowingObserver : public mdbxc::sync::ISyncTransportObserver {
public:
    void on_sync_transport_pull_result(
            const mdbxc::sync::PullRequest& request,
            const mdbxc::sync::PullResponse& response) override {
        (void)request;
        (void)response;
        throw std::runtime_error("observer failure");
    }

    void on_sync_transport_rejected(
            mdbxc::sync::SyncTransportOperation operation,
            const std::string& error) override {
        (void)operation;
        (void)error;
        throw std::runtime_error("observer rejection failure");
    }
};

void test_peer_middleware_allows_limits_and_observes() {
    const mdbxc::sync::NodeId requester = make_node(0x10);
    const mdbxc::sync::DbId db_id = make_node(0x20);

    mdbxc::sync::NodeDbAllowListPolicy allow_list;
    allow_list.allow_node_id(requester);
    allow_list.allow_db_id(db_id);

    mdbxc::sync::FixedBudgetSyncTransportPolicy budget(1, 1);
    mdbxc::sync::CompositeSyncTransportPolicy policy;
    policy.add(allow_list);
    policy.add(budget);

    mdbxc::sync::SyncTransportMetricsObserver metrics;
    RecordingPeer peer;
    mdbxc::sync::SyncPeerMiddleware wrapped(peer, &policy, &metrics);

    mdbxc::sync::PullRequest pull;
    pull.requester = requester;
    pull.db_id = db_id;
    mdbxc::sync::PullResponse pulled = wrapped.pull(pull);
    require_true(pulled.ok, "allowed pull was rejected");
    require_true(peer.pull_count() == 1u, "allowed pull was not forwarded");

    pulled = wrapped.pull(pull);
    require_true(!pulled.ok, "rate-limited pull was allowed");
    require_true(peer.pull_count() == 1u,
                 "rate-limited pull reached downstream peer");

    mdbxc::sync::PushRequest push;
    push.sender = requester;
    push.db_id = db_id;
    push.batches.push_back(make_batch(0x10, 1));
    mdbxc::sync::PushResponse pushed = wrapped.push(push);
    require_true(pushed.ok, "allowed push was rejected");
    require_true(peer.push_count() == 1u, "allowed push was not forwarded");

    push.db_id = make_node(0x99);
    pushed = wrapped.push(push);
    require_true(!pushed.ok, "db-denied push was allowed");
    require_true(peer.push_count() == 1u,
                 "db-denied push reached downstream peer");

    wrapped.request_cancel();
    require_true(peer.cancel_count() == 1u,
                 "request_cancel was not forwarded");

    const mdbxc::sync::SyncTransportMetricsSnapshot snapshot =
        metrics.snapshot();
    require_true(snapshot.pull_calls == 1u, "pull metric mismatch");
    require_true(snapshot.push_calls == 1u, "push metric mismatch");
    require_true(snapshot.rejected_calls == 2u, "rejection metric mismatch");
    require_true(snapshot.request_cancel_calls == 1u,
                 "cancel metric mismatch");
    require_true(snapshot.pulled_batches == 1u,
                 "pulled batch metric mismatch");
    require_true(snapshot.pushed_batches == 1u,
                 "pushed batch metric mismatch");
}

void test_observer_exceptions_are_swallowed() {
    RecordingPeer peer;
    ThrowingObserver observer;
    mdbxc::sync::SyncPeerMiddleware wrapped(peer, nullptr, &observer);

    mdbxc::sync::PullRequest pull;
    pull.requester = make_node(0x50);
    pull.db_id = make_node(0x51);

    const mdbxc::sync::PullResponse response = wrapped.pull(pull);
    require_true(response.ok, "observer exception changed pull result");
    require_true(peer.pull_count() == 1u,
                 "observer exception prevented downstream pull");

    mdbxc::sync::NodeDbAllowListPolicy deny_db;
    deny_db.allow_db_id(make_node(0x99));
    mdbxc::sync::SyncPeerMiddleware denying(peer, &deny_db, &observer);
    const mdbxc::sync::PullResponse denied = denying.pull(pull);
    require_true(!denied.ok, "observer rejection exception allowed pull");
    require_true(peer.pull_count() == 1u,
                 "denied pull reached downstream peer");
}

void test_http_client_middleware_route_policy() {
    mdbxc::sync::HttpRouteAllowListPolicy route_policy;
    route_policy.allow_target(mdbxc::sync::HttpSyncRoutes::pull_target());

    mdbxc::sync::SyncTransportMetricsObserver metrics;
    RecordingHttpClient client;
    mdbxc::sync::HttpSyncClientMiddleware wrapped(
        client, &route_policy, &metrics);

    const std::vector<std::uint8_t> body(3u, 0x42u);
    mdbxc::sync::HttpSyncResponse response = wrapped.post(
        mdbxc::sync::HttpSyncRoutes::pull_target(),
        mdbxc::sync::HttpSyncRoutes::content_type(),
        body,
        mdbxc::sync::CancellationToken());
    require_true(response.status_code == 200, "allowed HTTP post failed");
    require_true(client.post_count() == 1u, "HTTP post was not forwarded");

    response = wrapped.post(
        "/forbidden",
        mdbxc::sync::HttpSyncRoutes::content_type(),
        body,
        mdbxc::sync::CancellationToken());
    require_true(response.status_code == 403,
                 "forbidden HTTP route was not rejected");
    require_true(client.post_count() == 1u,
                 "forbidden HTTP route reached downstream client");

    wrapped.request_cancel();
    require_true(client.cancel_count() == 1u,
                 "HTTP request_cancel was not forwarded");

    const mdbxc::sync::SyncTransportMetricsSnapshot snapshot =
        metrics.snapshot();
    require_true(snapshot.http_post_calls == 1u,
                 "HTTP post metric mismatch");
    require_true(snapshot.rejected_calls == 1u,
                 "HTTP rejection metric mismatch");
    require_true(snapshot.request_cancel_calls == 1u,
                 "HTTP cancel metric mismatch");
}

void test_http_client_middleware_budget_policy() {
    mdbxc::sync::FixedBudgetSyncTransportPolicy budget(
        mdbxc::sync::FixedBudgetSyncTransportPolicy::unlimited_budget(),
        mdbxc::sync::FixedBudgetSyncTransportPolicy::unlimited_budget(),
        1);
    RecordingHttpClient client;
    mdbxc::sync::HttpSyncClientMiddleware wrapped(client, &budget);

    const std::vector<std::uint8_t> body(1u, 0x11u);
    mdbxc::sync::HttpSyncResponse response = wrapped.post(
        mdbxc::sync::HttpSyncRoutes::pull_target(),
        mdbxc::sync::HttpSyncRoutes::content_type(),
        body,
        mdbxc::sync::CancellationToken());
    require_true(response.status_code == 200,
                 "first budgeted HTTP post failed");

    response = wrapped.post(
        mdbxc::sync::HttpSyncRoutes::pull_target(),
        mdbxc::sync::HttpSyncRoutes::content_type(),
        body,
        mdbxc::sync::CancellationToken());
    require_true(response.status_code == 429,
                 "rate-limited HTTP post was not rejected");
    require_true(client.post_count() == 1u,
                 "rate-limited HTTP post reached downstream client");
}

} // namespace

int main() {
    test_peer_middleware_allows_limits_and_observes();
    test_observer_exceptions_are_swallowed();
    test_http_client_middleware_route_policy();
    test_http_client_middleware_budget_policy();
    return 0;
}
