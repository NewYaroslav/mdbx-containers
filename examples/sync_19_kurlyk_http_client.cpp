/**
 * \ingroup mdbxc_examples
 * \brief HTTP sync client backend over Kurlyk/libcurl.
 *
 * This example keeps the server side on the ready Simple-Web HTTP listener and
 * swaps only the client-side socket backend to Kurlyk. The sync API above the
 * transport stays the same: KurlykHttpSyncClient implements IHttpSyncClient,
 * HttpSyncPeer turns it into an ISyncPeer, and the local replica applies the
 * pulled page with SyncEngine::handle_push().
 *
 * Build it explicitly:
 *
 *   cmake -S . -B tmp/build-kurlyk-http \
 *       -DMDBXC_DEPS_MODE=BUNDLED \
 *       -DMDBXC_BUILD_TESTS=OFF \
 *       -DMDBXC_BUILD_EXAMPLES=ON \
 *       -DMDBXC_KURLYK_HTTP_SYNC_EXAMPLE=ON \
 *       -DCMAKE_CXX_STANDARD=17
 *   cmake --build tmp/build-kurlyk-http \
 *       --target sync_19_kurlyk_http_client
 *
 * Run:
 *
 *   ./tmp/build-kurlyk-http/bin/examples/sync_19_kurlyk_http_client
 *
 * Expected output:
 *
 *   [server] listening on http://127.0.0.1:<port>
 *   [client] Kurlyk HTTP pull applied 2 batch(es)
 *   [client] request_cancel forwarded once
 *   OK: sync_19_kurlyk_http_client
 */

#include <mdbx_containers/sync/transports/kurlyk/HttpTransport.hpp>
#include <mdbx_containers/sync/transports/simple_web/HttpTransport.hpp>
#include <mdbx_containers/tables.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

#include "sync_example_utils.hpp"

namespace {

const std::uint8_t kPrimaryNodeSeed = 0xC1;
const std::uint8_t kReplicaNodeSeed = 0xC2;
const std::uint8_t kDatabaseSeed = 0xC3;
const char* kBearerToken = "kurlyk-demo-token";

void seed_primary_rows(const std::shared_ptr<mdbxc::Connection>& db);
void require_quote(const std::shared_ptr<mdbxc::Connection>& db,
                   int key,
                   const std::string& expected);

} // namespace

int main() {
    const std::string primary_path = "sync_19_primary.mdbx";
    const std::string replica_path = "sync_19_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    try {
        const mdbxc::sync::NodeId primary_node =
            sync_example::make_node(kPrimaryNodeSeed);
        const mdbxc::sync::NodeId replica_node =
            sync_example::make_node(kReplicaNodeSeed);
        const mdbxc::sync::DbId db_id =
            sync_example::make_node(kDatabaseSeed);

        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica =
            sync_example::open(replica_path);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_engine(replica);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        seed_primary_rows(primary);

        mdbxc::sync::HttpSyncServer http_server(primary_engine);

        mdbxc::sync::HttpBearerNodeIdentityPolicy bearer_policy;
        bearer_policy.allow_token_for_node(kBearerToken, replica_node);
        bearer_policy.allow_db_id_for_token(kBearerToken, db_id);

        std::mutex server_mutex;
        mdbxc::sync::simple_web::HttpSyncListenerConfig server_config;
        server_config.host = "127.0.0.1";
        server_config.port = 0;
        server_config.handler_mutex = &server_mutex;

        mdbxc::sync::simple_web::HttpSyncListener listener(
            http_server, server_config, &bearer_policy);
        listener.start();
        std::printf("[server] listening on http://127.0.0.1:%u\n",
                    static_cast<unsigned>(listener.port()));

        mdbxc::sync::kurlyk::HttpSyncClientConfig client_config;
        client_config.base_url =
            std::string("http://127.0.0.1:") +
            std::to_string(static_cast<unsigned>(listener.port()));
        client_config.bearer_token = kBearerToken;
        client_config.wait_poll_interval = std::chrono::milliseconds(5);

        mdbxc::sync::kurlyk::HttpSyncClient raw_client(client_config);
        mdbxc::sync::HttpSyncPeer peer(raw_client);

        mdbxc::sync::PullRequest pull;
        pull.requester = replica_node;
        pull.db_id = db_id;
        pull.have = replica_engine.applied_cursor();
        pull.max_batches = 100;

        const mdbxc::sync::PullResponse pulled = peer.pull(pull);
        sync_example::require(pulled.ok,
                              "Kurlyk HTTP pull failed: " + pulled.error);

        mdbxc::sync::PushRequest local_apply;
        local_apply.sender = primary_node;
        local_apply.db_id = db_id;
        local_apply.batches = pulled.batches;
        const mdbxc::sync::PushResponse applied =
            replica_engine.handle_push(local_apply);
        sync_example::require(applied.ok,
                              "replica apply failed: " + applied.error);
        std::printf("[client] Kurlyk HTTP pull applied %zu batch(es)\n",
                    pulled.batches.size());

        require_quote(replica, 1, "BTC/USD");
        require_quote(replica, 2, "ETH/USD");

        peer.request_cancel();
        sync_example::require(raw_client.cancel_count() == 1u,
                              "request_cancel was not forwarded");
        std::printf("[client] request_cancel forwarded once\n");

        listener.stop();
        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::puts("OK: sync_19_kurlyk_http_client");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sync_19_kurlyk_http_client failed: %s\n",
                     e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}

namespace {

void seed_primary_rows(const std::shared_ptr<mdbxc::Connection>& db) {
    mdbxc::sync::ThreadLocalChangeAccumulator sink(db);
    mdbxc::KeyValueTable<int, std::string> quotes(db, "quotes");
    db->attach_sync_capture(&sink);
    quotes.insert_or_assign(1, "BTC/USD");
    quotes.insert_or_assign(2, "ETH/USD");
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
