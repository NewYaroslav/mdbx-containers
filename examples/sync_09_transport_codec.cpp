/**
 * \ingroup mdbxc_examples
 * \brief Transport DTO codec round-trip over byte buffers.
 *
 * This example shows the byte-level boundary that an HTTP/WebSocket adapter
 * will use:
 *   PullRequest DTO -> TransportMessageCodec bytes -> server decode
 *   -> SyncEngine::handle_pull() -> PullResponse bytes -> client decode
 *   -> local SyncEngine::handle_push().
 *
 * It also demonstrates the opposite direction for a push endpoint:
 *   PushRequest DTO -> bytes -> receiver decode -> handle_push()
 *   -> PushResponse bytes -> sender decode.
 *
 * The "wire" variables are plain std::vector<uint8_t>. A real transport would
 * write exactly those buffers to a request body, socket frame, message queue,
 * or another application-owned channel.
 *
 * Expected output:
 *   [transport codec pull] applied 2 batch(es)
 *   [transport codec push] applied 1 batch(es)
 *   OK: sync_09_transport_codec
 */

#include "sync_example_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

int main() {
    const std::string primary_path = "sync_09_primary.mdbx";
    const std::string replica_path = "sync_09_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const std::uint8_t primary_node_seed = 0xD0;
    const std::uint8_t replica_node_seed = 0xD1;
    const std::uint8_t logical_db_seed   = 0xD2;
    const std::uint64_t no_pagination_limit = 100;

    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(primary_node_seed);
    const mdbxc::sync::NodeId replica_node =
        sync_example::make_node(replica_node_seed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(logical_db_seed);

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

        // ---- Pull over encoded bytes ---------------------------------
        mdbxc::sync::PullRequest client_pull;
        client_pull.requester = replica_node;
        client_pull.db_id = db_id;
        client_pull.have = replica_engine.applied_cursor();
        client_pull.max_batches = no_pagination_limit;

        const std::vector<std::uint8_t> pull_request_wire =
            mdbxc::sync::TransportMessageCodec::encode_pull_request(
                client_pull);

        // Server side: the transport has delivered only bytes. Decode the DTO
        // and dispatch it to the local source engine.
        const mdbxc::sync::PullRequest server_pull =
            mdbxc::sync::TransportMessageCodec::decode_pull_request(
                pull_request_wire);
        const mdbxc::sync::PullResponse server_pull_response =
            primary_engine.handle_pull(server_pull);
        const std::vector<std::uint8_t> pull_response_wire =
            mdbxc::sync::TransportMessageCodec::encode_pull_response(
                server_pull_response);

        // Client side: decode the response and apply the returned page locally.
        const mdbxc::sync::PullResponse client_pull_response =
            mdbxc::sync::TransportMessageCodec::decode_pull_response(
                pull_response_wire);
        sync_example::require(
            client_pull_response.ok,
            "pull response failed: " + client_pull_response.error);
        sync_example::require(
            client_pull_response.batches.size() == 2u,
            "encoded pull expected two batches");

        mdbxc::sync::PushRequest local_apply;
        local_apply.sender = primary_node;
        local_apply.db_id = db_id;
        local_apply.batches = client_pull_response.batches;
        const mdbxc::sync::PushResponse local_apply_response =
            replica_engine.handle_push(local_apply);
        sync_example::require(
            local_apply_response.ok,
            "local apply failed: " + local_apply_response.error);
        std::printf("[transport codec pull] applied %zu batch(es)\n",
                    client_pull_response.batches.size());

        // ---- Push over encoded bytes ---------------------------------
        primary->attach_sync_capture(&capture);
        primary_ticks.insert_or_assign(3, "SOL/USD");
        primary->detach_sync_capture();

        // make_push_request() hides the changelog DBI details from adapter
        // code. A future HTTP/WebSocket sender can encode this DTO directly.
        const mdbxc::sync::PushRequest source_push =
            primary_engine.make_push_request(3, 0);
        sync_example::require(source_push.batches.size() == 1u,
                              "encoded push expected one batch");

        const std::vector<std::uint8_t> push_request_wire =
            mdbxc::sync::TransportMessageCodec::encode_push_request(
                source_push);
        const mdbxc::sync::PushRequest receiver_push =
            mdbxc::sync::TransportMessageCodec::decode_push_request(
                push_request_wire);
        const mdbxc::sync::PushResponse receiver_push_response =
            replica_engine.handle_push(receiver_push);
        const std::vector<std::uint8_t> push_response_wire =
            mdbxc::sync::TransportMessageCodec::encode_push_response(
                receiver_push_response);

        const mdbxc::sync::PushResponse source_push_response =
            mdbxc::sync::TransportMessageCodec::decode_push_response(
                push_response_wire);
        sync_example::require(
            source_push_response.ok,
            "push response failed: " + source_push_response.error);
        sync_example::require(
            source_push_response.receiver_have.last_seq_for(primary_node) == 3u,
            "push response cursor did not reach seq 3");
        std::printf("[transport codec push] applied %zu batch(es)\n",
                    receiver_push.batches.size());

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
        std::printf("OK: sync_09_transport_codec\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
