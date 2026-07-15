/**
 * \ingroup mdbxc_examples
 * \brief One primary database fans out the same changelog to two replicas.
 *
 * Every replica has its own persisted applied cursor. The primary does not mark
 * a batch as globally delivered: replica A and replica B can request and apply
 * the same primary sequence independently.
 *
 * Expected output:
 *   [primary -> replica A] applied 2 batch(es)
 *   [primary -> replica B] applied 2 batch(es)
 *   [primary -> replica A] applied 1 batch(es)
 *   [primary -> replica B] applied 1 batch(es)
 *   OK: sync_04_primary_to_replicas
 */

#include "sync_example_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace {

// Each call reads the target replica's own cursor, pulls from the primary, and
// applies the returned pages only to that replica. This is the fan-out rule:
// delivery state belongs to the receiver, not to the primary.
std::size_t sync_primary_to_replica(mdbxc::sync::SyncEngine& primary_engine,
                                    mdbxc::sync::SyncEngine& replica_engine,
                                    const mdbxc::sync::NodeId& primary_node,
                                    const mdbxc::sync::NodeId& replica_node,
                                    const mdbxc::sync::DbId& db_id,
                                    const char* replica_name) {
    mdbxc::sync::DirectSyncPeer primary_peer(&primary_engine);
    mdbxc::sync::PullRequest pull;
    pull.requester = replica_node;
    pull.db_id = db_id;
    pull.have = replica_engine.applied_cursor();
    pull.max_batches = 2;

    std::size_t applied = 0;
    bool has_more = false;
    do {
        const mdbxc::sync::SyncCursor before = pull.have;
        const mdbxc::sync::PullResponse pulled = primary_peer.pull(pull);
        sync_example::require(pulled.ok,
                              std::string(replica_name) +
                                  " pull failed: " + pulled.error);
        sync_example::require(!pulled.has_more || !pulled.batches.empty(),
                              std::string(replica_name) +
                                  " has_more without batches");

        if (!pulled.batches.empty()) {
            mdbxc::sync::PushRequest push;
            push.sender = primary_node;
            push.db_id = db_id;
            push.batches = pulled.batches;

            const mdbxc::sync::PushResponse pushed =
                replica_engine.handle_push(push);
            sync_example::require(pushed.ok,
                                  std::string(replica_name) +
                                      " apply failed: " + pushed.error);
            applied += pulled.batches.size();
            pull.have = pushed.receiver_have;
        } else {
            pull.have = replica_engine.applied_cursor();
        }

        has_more = pulled.has_more;
        sync_example::require(!has_more ||
                                  pull.have.last_seq_by_origin !=
                                      before.last_seq_by_origin,
                              std::string(replica_name) +
                                  " pagination made no cursor progress");
    } while (has_more);

    std::printf("[primary -> %s] applied %zu batch(es)\n",
                replica_name, applied);
    return applied;
}

void check_replica(const std::shared_ptr<mdbxc::Connection>& replica,
                   const std::string& name) {
    mdbxc::KeyValueTable<int, std::string> ticks(replica, "ticks");
    sync_example::require(
        sync_example::kv_or_throw(replica, ticks, 1001,
                                  name + " ticks[1001]") == "BTC 65000",
        name + " first tick mismatch");
    sync_example::require(
        sync_example::kv_or_throw(replica, ticks, 1002,
                                  name + " ticks[1002]") == "ETH 3500",
        name + " second tick mismatch");
}

} // namespace

int main() {
    const std::string primary_path = "sync_04_primary.mdbx";
    const std::string replica_a_path = "sync_04_replica_a.mdbx";
    const std::string replica_b_path = "sync_04_replica_b.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_a_path);
    sync_example::cleanup(replica_b_path);

    const std::uint8_t primary_node_seed = 0x70;
    const std::uint8_t replica_a_node_seed = 0x80;
    const std::uint8_t replica_b_node_seed = 0x90;
    const std::uint8_t logical_db_seed = 0xD3;
    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(primary_node_seed);
    const mdbxc::sync::NodeId replica_a_node =
        sync_example::make_node(replica_a_node_seed);
    const mdbxc::sync::NodeId replica_b_node =
        sync_example::make_node(replica_b_node_seed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(logical_db_seed);
    const std::size_t expected_initial_batches = 2u;
    const std::size_t expected_incremental_batches = 1u;

    try {
        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica_a =
            sync_example::open(replica_a_path);
        std::shared_ptr<mdbxc::Connection> replica_b =
            sync_example::open(replica_b_path);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_a_engine(replica_a);
        mdbxc::sync::SyncEngine replica_b_engine(replica_b);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_a_engine.initialize_local_identity(replica_a_node, db_id);
        replica_b_engine.initialize_local_identity(replica_b_node, db_id);

        mdbxc::sync::ThreadLocalChangeAccumulator capture(primary);
        primary->attach_sync_capture(&capture);
        mdbxc::KeyValueTable<int, std::string> ticks(primary, "ticks");
        ticks.insert_or_assign(1001, "BTC 65000");
        ticks.insert_or_assign(1002, "ETH 3500");
        primary->detach_sync_capture();

        const std::size_t applied_a = sync_primary_to_replica(
            primary_engine, replica_a_engine, primary_node, replica_a_node,
            db_id, "replica A");
        const std::size_t applied_b = sync_primary_to_replica(
            primary_engine, replica_b_engine, primary_node, replica_b_node,
            db_id, "replica B");
        sync_example::require(applied_a == expected_initial_batches &&
                                  applied_b == expected_initial_batches,
                              "fan-out expected two batches per replica");

        check_replica(replica_a, "replica A");
        check_replica(replica_b, "replica B");

        primary->attach_sync_capture(&capture);
        ticks.insert_or_assign(1003, "SOL 180");
        primary->detach_sync_capture();

        // After the first sync both replicas have primary sequence 2 in their
        // own applied cursor. The primary tail is now sequence 3, so each next
        // pull returns exactly one new batch.
        sync_example::require(
            sync_primary_to_replica(primary_engine, replica_a_engine,
                                    primary_node, replica_a_node, db_id,
                                    "replica A") ==
                expected_incremental_batches,
            "incremental sync to replica A expected one batch");
        sync_example::require(
            sync_primary_to_replica(primary_engine, replica_b_engine,
                                    primary_node, replica_b_node, db_id,
                                    "replica B") ==
                expected_incremental_batches,
            "incremental sync to replica B expected one batch");

        mdbxc::KeyValueTable<int, std::string> ticks_a(replica_a, "ticks");
        mdbxc::KeyValueTable<int, std::string> ticks_b(replica_b, "ticks");
        sync_example::require(
            sync_example::kv_or_throw(replica_a, ticks_a, 1003,
                                      "replica A ticks[1003]") == "SOL 180",
            "replica A incremental tick mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica_b, ticks_b, 1003,
                                      "replica B ticks[1003]") == "SOL 180",
            "replica B incremental tick mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica_a, replica_a_path);
        sync_example::disconnect_and_cleanup(replica_b, replica_b_path);
        std::printf("OK: sync_04_primary_to_replicas\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_a_path);
        sync_example::cleanup(replica_b_path);
        return 1;
    }
}
