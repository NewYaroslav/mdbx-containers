/**
 * \ingroup mdbxc_examples
 * \brief Incremental sync with the receiver's persisted cursor.
 *
 * This example repeats the direct pull/apply lifecycle twice:
 *   1. the replica starts with an empty cursor and receives two batches;
 *   2. the primary commits one more write;
 *   3. the replica sends its persisted cursor and receives only the new batch.
 *
 * Expected output:
 *   [incremental first pull] applied 2 batch(es)
 *   [incremental second pull] applied 1 batch(es)
 *   OK: sync_02_incremental_direct_peer
 */

#include "sync_example_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

int main() {
    const std::string primary_path = "sync_02_primary.mdbx";
    const std::string replica_path = "sync_02_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const std::uint8_t primary_node_seed = 0x30;
    const std::uint8_t replica_node_seed = 0x40;
    const std::uint8_t logical_db_seed = 0xD1;
    const std::size_t expected_first_pull_batches = 2u;
    const std::size_t expected_second_pull_batches = 1u;

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
        mdbxc::KeyValueTable<int, std::string> ticks(primary, "ticks");

        primary->attach_sync_capture(&capture);
        ticks.insert_or_assign(1001, "BTC 65000");
        ticks.insert_or_assign(1002, "ETH 3500");
        primary->detach_sync_capture();

        mdbxc::sync::DirectSyncPeer primary_peer(&primary_engine);

        mdbxc::sync::PullRequest first_pull;
        first_pull.requester = replica_node;
        first_pull.db_id = db_id;
        // Empty applied cursor means the replica has not applied anything yet.
        first_pull.have = replica_engine.applied_cursor();
        first_pull.max_batches = 100;

        const mdbxc::sync::PullResponse first_page =
            primary_peer.pull(first_pull);
        sync_example::require(first_page.ok,
                              "first pull failed: " + first_page.error);
        sync_example::require(
            first_page.batches.size() == expected_first_pull_batches,
            "first pull expected two batches");

        mdbxc::sync::PushRequest first_push;
        first_push.sender = primary_node;
        first_push.db_id = db_id;
        first_push.batches = first_page.batches;
        const mdbxc::sync::PushResponse first_apply =
            replica_engine.handle_push(first_push);
        sync_example::require(first_apply.ok,
                              "first apply failed: " + first_apply.error);
        std::printf("[incremental first pull] applied %zu batch(es)\n",
                    first_page.batches.size());

        primary->attach_sync_capture(&capture);
        ticks.insert_or_assign(1003, "SOL 180");
        primary->detach_sync_capture();

        mdbxc::sync::PullRequest second_pull;
        second_pull.requester = replica_node;
        second_pull.db_id = db_id;
        // The replica's persisted cursor now contains primary sequence 2, so
        // the next pull skips the two already applied batches.
        second_pull.have = replica_engine.applied_cursor();
        second_pull.max_batches = 100;

        const mdbxc::sync::PullResponse second_page =
            primary_peer.pull(second_pull);
        sync_example::require(second_page.ok,
                              "second pull failed: " + second_page.error);
        sync_example::require(
            second_page.batches.size() == expected_second_pull_batches,
            "second pull expected one incremental batch");

        mdbxc::sync::PushRequest second_push;
        second_push.sender = primary_node;
        second_push.db_id = db_id;
        second_push.batches = second_page.batches;
        const mdbxc::sync::PushResponse second_apply =
            replica_engine.handle_push(second_push);
        sync_example::require(second_apply.ok,
                              "second apply failed: " + second_apply.error);
        std::printf("[incremental second pull] applied %zu batch(es)\n",
                    second_page.batches.size());

        mdbxc::KeyValueTable<int, std::string> replica_ticks(replica,
                                                             "ticks");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 1001,
                                      "ticks[1001]") == "BTC 65000",
            "replica first tick mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 1002,
                                      "ticks[1002]") == "ETH 3500",
            "replica second tick mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 1003,
                                      "ticks[1003]") == "SOL 180",
            "replica incremental tick mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_02_incremental_direct_peer\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
