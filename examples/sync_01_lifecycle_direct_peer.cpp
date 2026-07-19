/**
 * \ingroup mdbxc_examples
 * \brief One explicit sync lifecycle cycle with DirectSyncPeer.
 *
 * This example intentionally spells out the pull/apply protocol instead of
 * hiding it behind a helper:
 *   primary write -> PullRequest -> DirectSyncPeer::pull()
 *   -> PushRequest -> replica SyncEngine::handle_push() -> replica read.
 *
 * Expected output:
 *   OK: sync_01_lifecycle_direct_peer
 */

#include "sync_example_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

int main() {
    const std::string primary_path = "sync_01_primary.mdbx";
    const std::string replica_path = "sync_01_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    // Demo-only deterministic IDs. In a real application these 16-byte
    // identifiers should be generated once, persisted, and reused on restart.
    const std::uint8_t primary_node_seed = 0x10;
    const std::uint8_t replica_node_seed = 0x20;
    const std::uint8_t logical_db_seed = 0xD0;

    // NodeId identifies a physical participant. Each database copy gets its
    // own node id.
    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(primary_node_seed);
    const mdbxc::sync::NodeId replica_node =
        sync_example::make_node(replica_node_seed);

    // DbId identifies the logical replicated database. It must be the same on
    // both sides, otherwise pull/push requests are rejected as db_id mismatch.
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
        // Capture is opt-in. While attached, committed writes on supported
        // tables are appended to the primary's _mdbxc_changelog. The scope
        // restores the previous capture sink when the write phase ends.
        mdbxc::KeyValueTable<int, std::string> prices(primary, "prices");
        {
            mdbxc::sync::SyncCaptureScope capture_scope(primary, capture);
            prices.insert_or_assign(1, "BTC/USD");
            prices.insert_or_assign(2, "ETH/USD");
        }

        // DirectSyncPeer is an in-process stand-in for a transport. It forwards
        // pull() directly to primary_engine.handle_pull(). A real peer would
        // serialize PullRequest over HTTP/WebSocket/etc. and deserialize
        // PullResponse on the replica side.
        mdbxc::sync::DirectSyncPeer primary_peer(&primary_engine);

        mdbxc::sync::PullRequest pull;
        // The requester is the node asking for data. The primary can use it
        // for diagnostics or future transport policy.
        pull.requester = replica_node;
        // Both peers must agree which logical database is being replicated.
        pull.db_id = db_id;
        // The applied cursor says what this replica has already applied. Empty
        // cursor means "send from the beginning".
        pull.have = replica_engine.applied_cursor();
        // Response page limit. This example expects only two batches, so 100
        // avoids pagination. Lower values are useful for paginated examples.
        pull.max_batches = 100;

        const mdbxc::sync::PullResponse pulled = primary_peer.pull(pull);
        sync_example::require(pulled.ok, "pull failed: " + pulled.error);
        sync_example::require(pulled.batches.size() == 2u,
                              "expected two pulled batches");

        mdbxc::sync::PushRequest push;
        // Sender is the node that supplied this page. In this direct example
        // it is the primary node.
        push.sender = primary_node;
        push.db_id = db_id;
        // These ChangeBatch objects are the transport payload. handle_push()
        // applies them atomically in a local writable transaction.
        push.batches = pulled.batches;

        const mdbxc::sync::PushResponse applied =
            replica_engine.handle_push(push);
        sync_example::require(applied.ok,
                              "replica apply failed: " + applied.error);
        sync_example::require(
            applied.receiver_have.last_seq_for(primary_node) == 2u,
            "replica cursor did not advance");

        mdbxc::KeyValueTable<int, std::string> replica_prices(replica,
                                                              "prices");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_prices, 1,
                                      "prices[1]") == "BTC/USD",
            "replica prices[1] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_prices, 2,
                                      "prices[2]") == "ETH/USD",
            "replica prices[2] mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_01_lifecycle_direct_peer\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
