/**
 * \ingroup mdbxc_examples
 * \brief One primary replicates several supported table types with pagination.
 *
 * The example writes through three supported table APIs:
 *   - KeyValueTable: two insert_or_assign() calls;
 *   - ValueTable: one set() call;
 *   - SequenceTable: two append() calls.
 *
 * Each public write commits one local ChangeBatch in this example, so the
 * receiver expects five batches. max_batches is intentionally set to 1 to show
 * how a replica keeps pulling pages until the primary reports has_more=false.
 *
 * Expected output:
 *   [multi-table page] applied 1 batch(es), has_more=true
 *   [multi-table page] applied 1 batch(es), has_more=true
 *   [multi-table page] applied 1 batch(es), has_more=true
 *   [multi-table page] applied 1 batch(es), has_more=true
 *   [multi-table page] applied 1 batch(es), has_more=false
 *   OK: sync_03_multi_table
 */

#include "sync_example_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

int main() {
    const std::string primary_path = "sync_03_primary.mdbx";
    const std::string replica_path = "sync_03_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const std::size_t expected_symbol_batches = 2u;
    const std::size_t expected_schema_batches = 1u;
    const std::size_t expected_event_batches = 2u;
    const std::size_t expected_total_batches =
        expected_symbol_batches + expected_schema_batches +
        expected_event_batches;
    const std::uint8_t primary_node_seed = 0x50;
    const std::uint8_t replica_node_seed = 0x60;
    const std::uint8_t logical_db_seed = 0xD2;

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
        primary->attach_sync_capture(&capture);

        // Two KeyValueTable writes demonstrate ordinary keyed records.
        mdbxc::KeyValueTable<int, std::string> symbols_primary(primary,
                                                               "symbols");
        symbols_primary.insert_or_assign(1, "BTC");
        symbols_primary.insert_or_assign(2, "ETH");

        // ValueTable stores one singleton value under its internal key.
        mdbxc::ValueTable<int> schema_version_primary(primary,
                                                      "schema_version");
        schema_version_primary.set(7);

        // SequenceTable appends generate physical integer keys on the primary.
        mdbxc::SequenceTable<std::string> events_primary(primary, "events");
        events_primary.append("connected");
        events_primary.append("subscribed");
        primary->detach_sync_capture();

        mdbxc::sync::DirectSyncPeer primary_peer(&primary_engine);
        mdbxc::sync::PullRequest pull;
        pull.requester = replica_node;
        pull.db_id = db_id;
        pull.have = replica_engine.applied_cursor();
        pull.max_batches = 1;

        // Pagination loop:
        //   1. ask the primary for batches after replica's cursor;
        //   2. apply one returned page on the replica;
        //   3. continue from the persisted cursor returned by handle_push();
        //   4. repeat while has_more is true.
        //
        // Each page is applied atomically, but the whole paginated pull is not
        // one global transaction. If a later page fails, earlier pages remain
        // applied and the persisted cursor tells the next attempt where to
        // resume.
        std::size_t applied = 0;
        bool has_more = false;
        do {
            const mdbxc::sync::SyncCursor before = pull.have;
            const mdbxc::sync::PullResponse pulled = primary_peer.pull(pull);
            sync_example::require(pulled.ok,
                                  "pull failed: " + pulled.error);
            sync_example::require(!pulled.has_more ||
                                      !pulled.batches.empty(),
                                  "has_more without batches");

            if (!pulled.batches.empty()) {
                mdbxc::sync::PushRequest push;
                push.sender = primary_node;
                push.db_id = db_id;
                push.batches = pulled.batches;

                const mdbxc::sync::PushResponse pushed =
                    replica_engine.handle_push(push);
                sync_example::require(pushed.ok,
                                      "push apply failed: " + pushed.error);
                applied += pulled.batches.size();
                pull.have = pushed.receiver_have;
            } else {
                pull.have = replica_engine.applied_cursor();
            }

            std::printf("[multi-table page] applied %zu batch(es), "
                        "has_more=%s\n",
                        pulled.batches.size(),
                        pulled.has_more ? "true" : "false");
            has_more = pulled.has_more;
            sync_example::require(!has_more ||
                                      pull.have.last_seq_by_origin !=
                                          before.last_seq_by_origin,
                                  "pagination made no cursor progress");
        } while (has_more);

        sync_example::require(applied == expected_total_batches,
                              "multi-table example expected five batches");

        mdbxc::KeyValueTable<int, std::string> symbols(replica, "symbols");
        sync_example::require(
            sync_example::kv_or_throw(replica, symbols, 1,
                                      "symbols[1]") == "BTC",
            "replica symbols[1] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, symbols, 2,
                                      "symbols[2]") == "ETH",
            "replica symbols[2] mismatch");

        mdbxc::ValueTable<int> schema_version(replica, "schema_version");
        const std::pair<bool, int> version = schema_version.find_compat();
        sync_example::require(version.first && version.second == 7,
                              "replica schema version mismatch");

        mdbxc::SequenceTable<std::string> events(replica, "events");
        sync_example::require(events.count() == 2u,
                              "replica events count mismatch");
        sync_example::require(
            sync_example::sequence_or_throw(replica, events, 0,
                                            "events[0]") == "connected",
            "replica events[0] mismatch");
        sync_example::require(
            sync_example::sequence_or_throw(replica, events, 1,
                                            "events[1]") == "subscribed",
            "replica events[1] mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_03_multi_table\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
