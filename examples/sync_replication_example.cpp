/**
 * \ingroup mdbxc_examples
 * \brief Multi-table replication round-trip via the SyncEngine + DirectSyncPeer.
 *
 * Three heterogeneous tables (two KeyValueTable instances + one
 * SequenceTable) are written on the primary, then replicated to a
 * replica through the sync engine. Verifies that captured changes flow
 * back through the changelog, the engine, and the peer, and that the
 * replica ends up with byte-identical state for every replicated table.
 */

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId n{};
    for (int i = 0; i < 16; ++i) n[i] = static_cast<std::uint8_t>(seed + i);
    return n;
}

mdbxc::Config cfg(const std::string& path, std::size_t max_dbs) {
    mdbxc::Config c;
    c.pathname = path;
    c.max_dbs = max_dbs;
    c.no_subdir = true;
    return c;
}

void cleanup(const std::string& p) {
    std::remove(p.c_str());
    std::remove((p + "-lck").c_str());
}

template<class KVT>
typename KVT::value_type::second_type kv_or_throw(
        const std::shared_ptr<mdbxc::Connection>& conn,
        KVT& kv, const typename KVT::value_type::first_type& key,
        const char* what) {
    typename KVT::value_type::second_type out{};
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    if (!kv.try_get(key, out, txn.handle())) {
        throw std::runtime_error(std::string("missing: ") + what);
    }
    return out;
}

} // namespace

int main() {
    using namespace mdbxc;
    using namespace mdbxc::sync;

    const std::string primary_path = "sync_replication_primary.mdbx";
    const std::string replica_path = "sync_replication_replica.mdbx";
    cleanup(primary_path);
    cleanup(replica_path);

    const NodeId primary_node = make_node(0xA0);
    const NodeId replica_node = make_node(0xB0);
    const NodeId db_uuid      = make_node(0xD0);

    auto primary_conn = Connection::create(cfg(primary_path, 16));
    auto replica_conn = Connection::create(cfg(replica_path, 16));

    SyncEngine primary_engine(primary_conn);
    SyncEngine replica_engine(replica_conn);
    // Both physical databases represent the same logical database,
    // so they use the same db_uuid but different node_id values.
    primary_engine.initialize_local_identity(primary_node, db_uuid);
    replica_engine.initialize_local_identity(replica_node, db_uuid);

    ThreadLocalChangeAccumulator primary_sink(primary_conn);
    // Enable local change capture on the primary.
    // Committed table operations will be appended to _mdbxc_changelog.
    primary_conn->attach_sync_capture(&primary_sink);

    std::printf("[primary] writing three tables\n");
    {
        KeyValueTable<int, std::string> kv_int(primary_conn, "kv_int");
        kv_int.insert_or_assign(1, "one");
        kv_int.insert_or_assign(2, "two");
        kv_int.insert_or_assign(3, "three");

        KeyValueTable<std::string, std::string> kv_str(primary_conn, "kv_str");
        kv_str.insert_or_assign("alpha", "first");
        kv_str.insert_or_assign("beta",  "second");

        SequenceTable<std::string> events(primary_conn, "events");
        events.append("boot");
        events.append("ready");
        events.append("serve");
    }
    primary_conn->detach_sync_capture();

    std::printf("[primary] replicating to replica via pull\n");
    // DirectSyncPeer is an in-process test transport that just calls
    // the remote engine's handle_pull directly. A real implementation
    // would replace this with HTTP/WebSocket/etc.
    DirectSyncPeer peer(&primary_engine);
    PullRequest pull;
    pull.requester = replica_node;
    pull.db_id     = db_uuid;
    // Empty `have` means the replica has no cursor yet, so the primary
    // returns its available changelog as an initial full pull.
    PullResponse pull_resp = peer.pull(pull);
    if (!pull_resp.ok) {
        std::printf("FAIL pull: %s\n", pull_resp.error.c_str());
        return 1;
    }
    std::printf("[primary] pulled %zu batches\n", pull_resp.batches.size());

    {
        // Apply all pulled batches in one transaction so initial replication
        // is atomic from the replica's point of view: either every batch
        // applies or none does.
        auto txn = replica_conn->transaction(TransactionMode::WRITABLE);
        for (const ChangeBatch& batch : pull_resp.batches) {
            const ApplyResult r = replica_engine.apply_batch(txn.handle(), batch);
            if (r != ApplyResult::Applied) {
                std::printf("FAIL: apply returned %d (expected Applied)\n",
                            static_cast<int>(r));
                return 1;
            }
        }
        txn.commit();
    }

    std::printf("[replica] verifying tables match primary\n");
    {
        KeyValueTable<int, std::string> kv_int(replica_conn, "kv_int");
        try {
            if (kv_or_throw(replica_conn, kv_int, 1, "kv_int[1]") != "one")   { std::printf("FAIL kv_int[1]\n");   return 1; }
            if (kv_or_throw(replica_conn, kv_int, 2, "kv_int[2]") != "two")   { std::printf("FAIL kv_int[2]\n");   return 1; }
            if (kv_or_throw(replica_conn, kv_int, 3, "kv_int[3]") != "three") { std::printf("FAIL kv_int[3]\n");   return 1; }
        } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }

        KeyValueTable<std::string, std::string> kv_str(replica_conn, "kv_str");
        try {
            if (kv_or_throw(replica_conn, kv_str, "alpha", "kv_str[alpha]") != "first") {
                std::printf("FAIL kv_str[alpha]\n"); return 1;
            }
            if (kv_or_throw(replica_conn, kv_str, "beta", "kv_str[beta]") != "second") {
                std::printf("FAIL kv_str[beta]\n"); return 1;
            }
        } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }

        SequenceTable<std::string> events(replica_conn, "events");
        if (events.count() != 3u) { std::printf("FAIL events.count\n"); return 1; }
    }

    std::printf("[primary] writing more, pushing to replica\n");
    primary_conn->attach_sync_capture(&primary_sink);
    {
        KeyValueTable<int, std::string> kv_int(primary_conn, "kv_int");
        kv_int.insert_or_assign(4, "four");

        SequenceTable<std::string> events(primary_conn, "events");
        events.append("drain");
    }
    primary_conn->detach_sync_capture();

    {
        // Build a PushRequest that covers only the batches the replica has
        // not seen yet. make_push_request opens its own short-lived read
        // transaction on the bound connection, so it is safe to call right
        // after a writable commit (no overlap with user's open txns).
        const std::uint64_t from_seq =
            replica_engine.applied_cursor().last_seq_for(primary_node) + 1;
        const PushRequest push = primary_engine.make_push_request(
            from_seq, /*to_seq=*/0);

        // The replica-side apply is performed inside an atomic writable
        // transaction by handle_push(); gap or conflict aborts the whole
        // push.
        DirectSyncPeer replica_peer(&replica_engine);
        const PushResponse push_resp = replica_peer.push(push);
        if (!push_resp.ok) { std::printf("FAIL push: %s\n", push_resp.error.c_str()); return 1; }
    }

    {
        KeyValueTable<int, std::string> kv_int(replica_conn, "kv_int");
        try {
            if (kv_or_throw(replica_conn, kv_int, 4, "kv_int[4] after push") != "four") {
                std::printf("FAIL kv_int[4] after push\n"); return 1;
            }
        } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
        SequenceTable<std::string> events(replica_conn, "events");
        if (events.count() != 4u) { std::printf("FAIL events.count after push\n"); return 1; }
    }

    std::printf("[replica] applied cursor: last_seq=%llu\n",
                static_cast<unsigned long long>(
                    replica_engine.applied_cursor().last_seq_for(primary_node)));

    primary_conn->disconnect();
    replica_conn->disconnect();
    cleanup(primary_path);
    cleanup(replica_path);

    std::printf("OK: multi-table replication round-trip\n");
    return 0;
}