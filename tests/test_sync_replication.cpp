/// \file test_sync_replication.cpp
/// \brief Multi-table replication scenarios for the sync engine.

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void cleanup(const std::string& p) { std::remove(p.c_str()); }

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

template<class KVT>
bool kv_has(const std::shared_ptr<mdbxc::Connection>& conn,
        KVT& kv, const typename KVT::value_type::first_type& key) {
    typename KVT::value_type::second_type out{};
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    return kv.try_get(key, out, txn.handle());
}

template<class KeyTableT, class KeyT>
bool key_has(const std::shared_ptr<mdbxc::Connection>& conn,
        KeyTableT& keys, const KeyT& key) {
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    return keys.contains(key, txn.handle());
}

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId n{};
    for (int i = 0; i < 16; ++i) n[i] = static_cast<std::uint8_t>(seed + i);
    return n;
}

mdbxc::Config cfg(const std::string& path) {
    mdbxc::Config c;
    c.pathname = path;
    c.max_dbs = 16;
    c.no_subdir = true;
    return c;
}

std::shared_ptr<mdbxc::Connection> open(const std::string& path) {
    return mdbxc::Connection::create(cfg(path));
}

std::size_t pull_all_to_replica(mdbxc::sync::SyncEngine& primary_engine,
                                mdbxc::sync::SyncEngine& replica_engine,
                                const mdbxc::sync::NodeId& replica_node,
                                const mdbxc::sync::NodeId& db_id,
                                std::uint64_t max_batches = 1000) {
    mdbxc::sync::DirectSyncPeer peer(&primary_engine);
    mdbxc::sync::PullRequest request;
    request.requester = replica_node;
    request.db_id = db_id;
    request.have = replica_engine.applied_cursor();
    request.max_batches = max_batches;

    std::size_t applied = 0;
    bool has_more = false;
    do {
        const mdbxc::sync::SyncCursor before = request.have;
        const mdbxc::sync::PullResponse response = peer.pull(request);
        if (!response.ok) {
            throw std::runtime_error("pull failed: " + response.error);
        }
        if (!response.batches.empty()) {
            mdbxc::sync::PushRequest push;
            push.sender = response.batches.front().origin_node_id;
            push.db_id = db_id;
            push.batches = response.batches;
            const mdbxc::sync::PushResponse pushed = replica_engine.handle_push(push);
            if (!pushed.ok) {
                throw std::runtime_error("push apply failed: " + pushed.error);
            }
            applied += response.batches.size();
        } else if (response.has_more) {
            throw std::runtime_error("pull reported has_more without batches");
        }

        has_more = response.has_more;
        request.have = replica_engine.applied_cursor();
        if (has_more &&
            request.have.last_seq_by_origin == before.last_seq_by_origin) {
            throw std::runtime_error("pull pagination made no cursor progress");
        }
    } while (has_more);

    return applied;
}

/// \brief Populates three different table types on \p conn while capture is on.
void write_three_tables(std::shared_ptr<mdbxc::Connection> conn) {
    using namespace mdbxc;
    KeyValueTable<int, std::string> kv_int(conn, "kv_int");
    kv_int.insert_or_assign(1, "one");
    kv_int.insert_or_assign(2, "two");

    KeyValueTable<std::string, std::string> kv_str(conn, "kv_str");
    kv_str.insert_or_assign("a", "alpha");
    kv_str.insert_or_assign("b", "beta");

    SequenceTable<std::string> events(conn, "events");
    events.append("e0");
    events.append("e1");
}

void verify_three_tables(std::shared_ptr<mdbxc::Connection> conn) {
    using namespace mdbxc;
    KeyValueTable<int, std::string> kv_int(conn, "kv_int");
    if (kv_or_throw(conn, kv_int, 1, "kv_int[1]") != "one")   throw std::runtime_error("kv_int[1]");
    if (kv_or_throw(conn, kv_int, 2, "kv_int[2]") != "two")   throw std::runtime_error("kv_int[2]");

    KeyValueTable<std::string, std::string> kv_str(conn, "kv_str");
    if (kv_or_throw(conn, kv_str, "a", "kv_str[a]") != "alpha") throw std::runtime_error("kv_str[a]");
    if (kv_or_throw(conn, kv_str, "b", "kv_str[b]") != "beta")  throw std::runtime_error("kv_str[b]");

    SequenceTable<std::string> events(conn, "events");
    if (events.count() != 2u) throw std::runtime_error("events.count");
}

void test_replication_pull_three_tables() {
    using namespace mdbxc;
    const std::string p = "test_rep_pull_three.mdbx";
    const std::string r = "test_rep_pull_three_replica.mdbx";
    cleanup(p); cleanup(r);

    auto primary = open(p);
    auto replica = open(r);
    sync::SyncEngine pe(primary), re(replica);
    pe.initialize_local_identity(make_node(0xA0), make_node(0xD0));
    re.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    sync::ThreadLocalChangeAccumulator sink(primary);
    primary->attach_sync_capture(&sink);
    write_three_tables(primary);
    primary->detach_sync_capture();

    sync::DirectSyncPeer peer(&pe);
    sync::PullRequest req;
    req.requester = make_node(0xB0);
    req.db_id     = make_node(0xD0);
    const sync::PullResponse resp = peer.pull(req);
    if (resp.batches.size() != 6u) {
        throw std::runtime_error("expected 6 batches, got " +
                                 std::to_string(resp.batches.size()));
    }

    {
        auto txn = replica->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& b : resp.batches) {
            if (re.apply_batch(txn.handle(), b) != sync::ApplyResult::Applied) {
                throw std::runtime_error("apply not Applied");
            }
        }
        txn.commit();
    }
    verify_three_tables(replica);

    primary->disconnect(); replica->disconnect();
    cleanup(p); cleanup(r);
}

void test_replication_push_three_tables() {
    using namespace mdbxc;
    const std::string p = "test_rep_push_three.mdbx";
    const std::string r = "test_rep_push_three_remote.mdbx";
    cleanup(p); cleanup(r);

    auto primary = open(p);
    auto remote  = open(r);
    sync::SyncEngine pe(primary), re(remote);
    pe.initialize_local_identity(make_node(0xA0), make_node(0xD0));
    re.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    sync::ThreadLocalChangeAccumulator sink(primary);
    primary->attach_sync_capture(&sink);
    write_three_tables(primary);
    primary->detach_sync_capture();

    sync::PushRequest req;
    req.sender = make_node(0xA0);
    req.db_id  = make_node(0xD0);
    {
        auto txn = primary->transaction(TransactionMode::WRITABLE);
        sync::MetaStore meta(primary->env_handle());
        meta.open(txn.handle());
        sync::ChangeLogStore log(primary->env_handle());
        log.open(txn.handle());
        const std::uint64_t last = meta.get_local_seq(txn.handle());
        std::vector<std::uint8_t> buf;
        for (std::uint64_t s = 1; s <= last; ++s) {
            if (!log.get(txn.handle(), make_node(0xA0), s, buf)) continue;
            req.batches.push_back(sync::ChangeBatchCodec::decode_exact(buf));
        }
        txn.commit();
    }

    sync::DirectSyncPeer peer(&re);
    const sync::PushResponse resp = peer.push(req);
    if (!resp.ok) throw std::runtime_error("push not ok: " + resp.error);
    verify_three_tables(remote);

    primary->disconnect(); remote->disconnect();
    cleanup(p); cleanup(r);
}

void test_replication_mixed_ops() {
    using namespace mdbxc;
    const std::string p = "test_rep_mixed.mdbx";
    const std::string r = "test_rep_mixed_replica.mdbx";
    cleanup(p); cleanup(r);

    auto primary = open(p);
    auto replica = open(r);
    sync::SyncEngine pe(primary), re(replica);
    pe.initialize_local_identity(make_node(0xA0), make_node(0xD0));
    re.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    sync::ThreadLocalChangeAccumulator sink(primary);
    primary->attach_sync_capture(&sink);
    {
        KeyValueTable<int, std::string> kv(primary, "kv_int");
        kv.insert_or_assign(1, "one");
        kv.insert_or_assign(2, "two");
        kv.insert_or_assign(3, "three");
        kv.erase(2);
    }
    primary->detach_sync_capture();

    sync::DirectSyncPeer peer(&pe);
    sync::PullRequest req; req.requester = make_node(0xB0); req.db_id = make_node(0xD0);
    const sync::PullResponse resp = peer.pull(req);

    auto txn = replica->transaction(TransactionMode::WRITABLE);
    for (const sync::ChangeBatch& b : resp.batches) {
        re.apply_batch(txn.handle(), b);
    }
    txn.commit();

    {
        KeyValueTable<int, std::string> kv(replica, "kv_int");
        if (kv_or_throw(replica, kv, 1, "kv[1]") != "one")   throw std::runtime_error("kv[1]");
        if (kv_has(replica, kv, 2))                            throw std::runtime_error("kv[2] still present");
        if (kv_or_throw(replica, kv, 3, "kv[3]") != "three") throw std::runtime_error("kv[3]");
    }

    primary->disconnect(); replica->disconnect();
    cleanup(p); cleanup(r);
}

void test_replication_value_table_singleton_key() {
    using namespace mdbxc;
    const std::string p = "test_rep_value_table.mdbx";
    const std::string r = "test_rep_value_table_replica.mdbx";
    cleanup(p); cleanup(r);

    auto primary = open(p);
    auto replica = open(r);
    sync::SyncEngine pe(primary), re(replica);
    pe.initialize_local_identity(make_node(0xA0), make_node(0xD0));
    re.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    sync::ThreadLocalChangeAccumulator sink(primary);
    sync::DirectSyncPeer peer(&pe);

    primary->attach_sync_capture(&sink);
    {
        ValueTable<int> state(primary, "state");
        state.set(42);
    }
    primary->detach_sync_capture();

    {
        sync::PullRequest req;
        req.requester = make_node(0xB0);
        req.db_id = make_node(0xD0);
        const sync::PullResponse resp = peer.pull(req);
        if (resp.batches.size() != 1u) {
            throw std::runtime_error("value table set expected one batch");
        }
        auto txn = replica->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& b : resp.batches) {
            if (re.apply_batch(txn.handle(), b) != sync::ApplyResult::Applied) {
                throw std::runtime_error("value table set apply failed");
            }
        }
        txn.commit();
    }

    {
        ValueTable<int> state(replica, "state");
        const std::pair<bool, int> found = state.find_compat();
        if (!found.first || found.second != 42) {
            throw std::runtime_error("value table set did not replicate");
        }
    }

    primary->attach_sync_capture(&sink);
    {
        ValueTable<int> state(primary, "state");
        if (!state.erase()) {
            throw std::runtime_error("value table erase unexpectedly returned false");
        }
    }
    primary->detach_sync_capture();

    {
        sync::PullRequest req;
        req.requester = make_node(0xB0);
        req.db_id = make_node(0xD0);
        const sync::SyncCursor cur = re.applied_cursor();
        for (const auto& kv : cur.last_seq_by_origin) {
            req.have.last_seq_by_origin[kv.first] = kv.second;
        }
        const sync::PullResponse resp = peer.pull(req);
        if (resp.batches.size() != 1u) {
            throw std::runtime_error("value table erase expected one batch");
        }
        auto txn = replica->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& b : resp.batches) {
            if (re.apply_batch(txn.handle(), b) != sync::ApplyResult::Applied) {
                throw std::runtime_error("value table erase apply failed");
            }
        }
        txn.commit();
    }

    {
        ValueTable<int> state(replica, "state");
        if (state.has_value()) {
            throw std::runtime_error("value table erase did not replicate");
        }
    }

    primary->disconnect(); replica->disconnect();
    cleanup(p); cleanup(r);
}

void test_replication_key_value_bulk_roundtrip() {
    using namespace mdbxc;
    const std::string p = "test_rep_kv_bulk_roundtrip.mdbx";
    const std::string r = "test_rep_kv_bulk_roundtrip_replica.mdbx";
    cleanup(p); cleanup(r);

    const sync::NodeId primary_node = make_node(0xA0);
    const sync::NodeId replica_node = make_node(0xB0);
    const sync::NodeId db_id = make_node(0xD0);

    std::shared_ptr<Connection> primary = open(p);
    std::shared_ptr<Connection> replica = open(r);

    {
        sync::SyncEngine pe(primary), re(replica);
        pe.initialize_local_identity(primary_node, db_id);
        re.initialize_local_identity(replica_node, db_id);

        sync::ThreadLocalChangeAccumulator sink(primary);
        primary->attach_sync_capture(&sink);

        {
            KeyValueTable<int, std::string> kv(primary, "bulk_kv");
            std::map<int, std::string> initial;
            initial[1] = "one";
            initial[2] = "two";
            initial[3] = "three";
            kv.append(initial);
        }
        if (pull_all_to_replica(pe, re, replica_node, db_id, 1) == 0u) {
            throw std::runtime_error("bulk append produced no replicated batches");
        }
        {
            KeyValueTable<int, std::string> kv(replica, "bulk_kv");
            if (kv_or_throw(replica, kv, 1, "bulk_kv[1]") != "one") {
                throw std::runtime_error("bulk append kv[1]");
            }
            if (kv_or_throw(replica, kv, 3, "bulk_kv[3]") != "three") {
                throw std::runtime_error("bulk append kv[3]");
            }
        }

        {
            KeyValueTable<int, std::string> kv(primary, "bulk_kv");
            std::vector<std::pair<int, std::string> > extra;
            extra.push_back(std::make_pair(2, "two updated"));
            extra.push_back(std::make_pair(4, "four"));
            kv.append(extra);
        }
        if (pull_all_to_replica(pe, re, replica_node, db_id, 1) == 0u) {
            throw std::runtime_error("vector append produced no replicated batches");
        }
        {
            KeyValueTable<int, std::string> kv(replica, "bulk_kv");
            if (kv_or_throw(replica, kv, 2, "bulk_kv[2]") != "two updated") {
                throw std::runtime_error("vector append did not update kv[2]");
            }
            if (kv_or_throw(replica, kv, 4, "bulk_kv[4]") != "four") {
                throw std::runtime_error("vector append did not add kv[4]");
            }
        }

        {
            KeyValueTable<int, std::string> kv(primary, "bulk_kv");
            std::map<int, std::string> replacement;
            replacement[2] = "two reconciled";
            replacement[4] = "four";
            replacement[5] = "five";
            kv.reconcile(replacement);
        }
        if (pull_all_to_replica(pe, re, replica_node, db_id, 1) == 0u) {
            throw std::runtime_error("reconcile produced no replicated batches");
        }
        {
            KeyValueTable<int, std::string> kv(replica, "bulk_kv");
            if (kv_has(replica, kv, 1)) {
                throw std::runtime_error("reconcile did not delete stale kv[1]");
            }
            if (kv_has(replica, kv, 3)) {
                throw std::runtime_error("reconcile did not delete stale kv[3]");
            }
            if (kv_or_throw(replica, kv, 2, "bulk_kv[2]") != "two reconciled") {
                throw std::runtime_error("reconcile did not update kv[2]");
            }
            if (kv_or_throw(replica, kv, 5, "bulk_kv[5]") != "five") {
                throw std::runtime_error("reconcile did not add kv[5]");
            }
        }

        {
            KeyValueTable<int, std::string> kv(primary, "bulk_kv");
            if (kv.erase_range(2, 4) != 2u) {
                throw std::runtime_error("kv erase_range removed wrong count");
            }
        }
        if (pull_all_to_replica(pe, re, replica_node, db_id, 1) == 0u) {
            throw std::runtime_error("kv erase_range produced no replicated batches");
        }
        {
            KeyValueTable<int, std::string> kv(replica, "bulk_kv");
            if (kv_has(replica, kv, 2) || kv_has(replica, kv, 4)) {
                throw std::runtime_error("kv erase_range deletes did not replicate");
            }
            if (kv_or_throw(replica, kv, 5, "bulk_kv[5]") != "five") {
                throw std::runtime_error("kv erase_range removed kv[5]");
            }
        }

        primary->detach_sync_capture();
    }

    primary->disconnect();
    replica->disconnect();
    primary.reset();
    replica.reset();

    primary = open(p);
    replica = open(r);
    {
        sync::SyncEngine pe(primary), re(replica);
        pe.initialize_local_identity(primary_node, db_id);
        re.initialize_local_identity(replica_node, db_id);

        {
            KeyValueTable<int, std::string> kv(replica, "bulk_kv");
            if (kv_has(replica, kv, 2) || kv_has(replica, kv, 4)) {
                throw std::runtime_error("restarted replica resurrected erased keys");
            }
            if (kv_or_throw(replica, kv, 5, "bulk_kv[5] after restart") != "five") {
                throw std::runtime_error("restarted replica lost kv[5]");
            }
        }

        sync::ThreadLocalChangeAccumulator sink(primary);
        primary->attach_sync_capture(&sink);
        {
            KeyValueTable<int, std::string> kv(primary, "bulk_kv");
            std::vector<std::pair<int, std::string> > after_restart;
            after_restart.push_back(std::make_pair(5, "five restarted"));
            after_restart.push_back(std::make_pair(6, "six"));
            kv.append(after_restart);
        }
        if (pull_all_to_replica(pe, re, replica_node, db_id, 1) == 0u) {
            throw std::runtime_error("restart append produced no replicated batches");
        }
        primary->detach_sync_capture();

        {
            KeyValueTable<int, std::string> kv(replica, "bulk_kv");
            if (kv_or_throw(replica, kv, 5, "bulk_kv[5] after restart sync") !=
                    "five restarted") {
                throw std::runtime_error("restart append did not update kv[5]");
            }
            if (kv_or_throw(replica, kv, 6, "bulk_kv[6] after restart sync") != "six") {
                throw std::runtime_error("restart append did not add kv[6]");
            }
            if (kv_has(replica, kv, 2) || kv_has(replica, kv, 4)) {
                throw std::runtime_error("restart incremental replay restored erased keys");
            }
        }
    }

    primary->disconnect();
    replica->disconnect();
    cleanup(p); cleanup(r);
}

void test_replication_key_table_range_delete_roundtrip() {
    using namespace mdbxc;
    const std::string p = "test_rep_key_range_roundtrip.mdbx";
    const std::string r = "test_rep_key_range_roundtrip_replica.mdbx";
    cleanup(p); cleanup(r);

    auto primary = open(p);
    auto replica = open(r);
    sync::SyncEngine pe(primary), re(replica);
    pe.initialize_local_identity(make_node(0xA0), make_node(0xD0));
    re.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    sync::ThreadLocalChangeAccumulator sink(primary);
    primary->attach_sync_capture(&sink);
    {
        KeyTable<int> keys(primary, "keys");
        std::vector<int> initial;
        initial.push_back(1);
        initial.push_back(2);
        initial.push_back(3);
        initial.push_back(4);
        initial.push_back(5);
        keys.append(initial);
    }
    if (pull_all_to_replica(pe, re, make_node(0xB0), make_node(0xD0), 1) == 0u) {
        throw std::runtime_error("key append produced no replicated batches");
    }

    {
        KeyTable<int> keys(replica, "keys");
        if (!key_has(replica, keys, 1) || !key_has(replica, keys, 5) ||
            keys.count() != 5u) {
            throw std::runtime_error("key append did not replicate");
        }
    }

    {
        KeyTable<int> keys(primary, "keys");
        if (keys.erase_range(2, 4) != 3u) {
            throw std::runtime_error("key erase_range removed wrong count");
        }
    }
    if (pull_all_to_replica(pe, re, make_node(0xB0), make_node(0xD0), 1) == 0u) {
        throw std::runtime_error("key erase_range produced no replicated batches");
    }
    primary->detach_sync_capture();

    {
        KeyTable<int> keys(replica, "keys");
        if (!key_has(replica, keys, 1) || !key_has(replica, keys, 5)) {
            throw std::runtime_error("key erase_range removed boundary survivors");
        }
        if (key_has(replica, keys, 2) || key_has(replica, keys, 3) ||
            key_has(replica, keys, 4)) {
            throw std::runtime_error("key erase_range deletes did not replicate");
        }
        if (keys.count() != 2u) {
            throw std::runtime_error("key erase_range left wrong count");
        }
    }

    primary->disconnect(); replica->disconnect();
    cleanup(p); cleanup(r);
}

void test_replication_incremental_pull() {
    using namespace mdbxc;
    const std::string p = "test_rep_incr.mdbx";
    const std::string r = "test_rep_incr_replica.mdbx";
    cleanup(p); cleanup(r);

    auto primary = open(p);
    auto replica = open(r);
    sync::SyncEngine pe(primary), re(replica);
    pe.initialize_local_identity(make_node(0xA0), make_node(0xD0));
    re.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    sync::ThreadLocalChangeAccumulator sink(primary);
    sync::DirectSyncPeer peer(&pe);

    // Round 1: 2 batches on primary
    primary->attach_sync_capture(&sink);
    {
        KeyValueTable<int, int> kv(primary, "kv");
        kv.insert_or_assign(1, 100);
        kv.insert_or_assign(2, 200);
    }
    primary->detach_sync_capture();

    {
        sync::PullRequest req; req.requester = make_node(0xB0); req.db_id = make_node(0xD0);
        const sync::PullResponse resp = peer.pull(req);
        if (resp.batches.size() != 2u) {
            throw std::runtime_error("round1 expected 2 batches");
        }
        auto txn = replica->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& b : resp.batches) re.apply_batch(txn.handle(), b);
        txn.commit();
    }

    // Round 2: 2 more batches on primary, replica's cursor now points at seq=2.
    primary->attach_sync_capture(&sink);
    {
        KeyValueTable<int, int> kv(primary, "kv");
        kv.insert_or_assign(3, 300);
        kv.insert_or_assign(4, 400);
    }
    primary->detach_sync_capture();

    {
        sync::PullRequest req;
        req.requester = make_node(0xB0);
        req.db_id     = make_node(0xD0);
        const sync::SyncCursor cur = re.applied_cursor();
        for (const auto& kv : cur.last_seq_by_origin) {
            req.have.last_seq_by_origin[kv.first] = kv.second;
        }
        const sync::PullResponse resp = peer.pull(req);
        if (resp.batches.size() != 2u) {
            throw std::runtime_error("round2 expected 2 batches, got " +
                                     std::to_string(resp.batches.size()));
        }
        auto txn = replica->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& b : resp.batches) re.apply_batch(txn.handle(), b);
        txn.commit();
    }

    {
        KeyValueTable<int, int> kv(replica, "kv");
        if (kv_or_throw(replica, kv, 1, "kv[1]") != 100) throw std::runtime_error("kv[1]");
        if (kv_or_throw(replica, kv, 4, "kv[4]") != 400) throw std::runtime_error("kv[4]");
    }

    primary->disconnect(); replica->disconnect();
    cleanup(p); cleanup(r);
}

void test_replication_make_push_request_helper() {
    using namespace mdbxc;
    const std::string p = "test_rep_make_push_helper.mdbx";
    const std::string r = "test_rep_make_push_helper_replica.mdbx";
    cleanup(p); cleanup(r);

    auto primary = open(p);
    auto replica = open(r);
    sync::SyncEngine pe(primary), re(replica);
    pe.initialize_local_identity(make_node(0xA0), make_node(0xD0));
    re.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    sync::ThreadLocalChangeAccumulator sink(primary);
    primary->attach_sync_capture(&sink);
    {
        KeyValueTable<int, std::string> kv(primary, "kv");
        kv.insert_or_assign(1, "a");
        kv.insert_or_assign(2, "b");
        kv.insert_or_assign(3, "c");
    }
    primary->detach_sync_capture();

    // Empty range -> empty request.
    {
        const sync::PushRequest empty = pe.make_push_request(
            /*from_seq=*/100, /*to_seq=*/0);
        if (!empty.batches.empty()) {
            throw std::runtime_error("empty range produced non-empty push");
        }
    }

    // Full window (from 1 to local tail).
    {
        const sync::PushRequest full = pe.make_push_request(
            /*from_seq=*/1, /*to_seq=*/0);
        if (full.batches.size() != 3u) {
            throw std::runtime_error("full window wrong size: " +
                                     std::to_string(full.batches.size()));
        }
        if (full.sender != make_node(0xA0) || full.db_id != make_node(0xD0)) {
            throw std::runtime_error("push request header fields wrong");
        }
        // End-to-end: helper -> DirectSyncPeer -> replica engine -> kv.
        sync::DirectSyncPeer peer(&re);
        const sync::PushResponse resp = peer.push(full);
        if (!resp.ok) { throw std::runtime_error("push via helper failed: " + resp.error); }
        KeyValueTable<int, std::string> kv_re(replica, "kv");
        if (kv_or_throw(replica, kv_re, 1, "kv[1] after helper push") != "a") throw std::runtime_error("kv[1] != a");
        if (kv_or_throw(replica, kv_re, 2, "kv[2] after helper push") != "b") throw std::runtime_error("kv[2] != b");
        if (kv_or_throw(replica, kv_re, 3, "kv[3] after helper push") != "c") throw std::runtime_error("kv[3] != c");
    }

    // Partial window (from 2 to 3).
    {
        const sync::PushRequest mid = pe.make_push_request(
            /*from_seq=*/2, /*to_seq=*/3);
        if (mid.batches.size() != 2u) {
            throw std::runtime_error("partial window wrong size: " +
                                     std::to_string(mid.batches.size()));
        }
    }

    // Reversed range (to_seq < from_seq) -> empty.
    {
        const sync::PushRequest rev = pe.make_push_request(
            /*from_seq=*/5, /*to_seq=*/2);
        if (!rev.batches.empty()) {
            throw std::runtime_error("reversed range produced non-empty push");
        }
    }

    // Gap in changelog -> exception, not silent skip.
    // Delete seq 2 directly via MDBX, then ask for [1, 3].
    {
        auto erase_txn = primary->transaction(mdbxc::TransactionMode::WRITABLE);
        sync::ChangeLogStore log(primary->env_handle());
        log.open(erase_txn.handle());
        const sync::NodeId local = make_node(0xA0);
        std::vector<std::uint8_t> key_buf(24);
        std::memcpy(key_buf.data(), local.data(), 16);
        const std::uint64_t seq = 2;
        for (int i = 0; i < 8; ++i) {
            key_buf[16 + i] = static_cast<std::uint8_t>((seq >> ((7 - i) * 8)) & 0xff);
        }
        MDBX_val k{ key_buf.data(), key_buf.size() };
        const int del_rc = mdbx_del(erase_txn.handle(), log.handle(), &k, nullptr);
        if (del_rc != MDBX_SUCCESS && del_rc != MDBX_NOTFOUND) {
            throw std::runtime_error("test setup: mdbx_del failed");
        }
        erase_txn.commit();

        bool threw = false;
        try {
            (void)pe.make_push_request(/*from_seq=*/1, /*to_seq=*/3);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            throw std::runtime_error("gap in changelog did not throw");
        }
    }

    primary->disconnect(); replica->disconnect();
    cleanup(p); cleanup(r);
}

void test_replication_idempotent_apply() {
    using namespace mdbxc;
    const std::string r = "test_rep_idempotent.mdbx";
    cleanup(r);

    auto conn = open(r);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0x10), make_node(0xD0));

    sync::ChangeBatch b;
    b.origin_node_id = make_node(0x20);
    b.seq = 1;
    sync::ChangeOp op;
    op.op_type = sync::ChangeOpType::Put;
    op.dbi_name = "t";
    op.storage_key = { 0x42 };
    op.value = { 0xAB };
    b.ops.push_back(op);

    auto txn = conn->transaction(TransactionMode::WRITABLE);
    if (engine.apply_batch(txn.handle(), b) != sync::ApplyResult::Applied) {
        throw std::runtime_error("first apply != Applied");
    }
    if (engine.apply_batch(txn.handle(), b) != sync::ApplyResult::Skipped) {
        throw std::runtime_error("second apply != Skipped");
    }
    if (engine.apply_batch(txn.handle(), b) != sync::ApplyResult::Skipped) {
        throw std::runtime_error("third apply != Skipped");
    }
    txn.commit();

    conn->disconnect();
    cleanup(r);
}

} // namespace

int main() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "test_replication_pull_three_tables",  &test_replication_pull_three_tables },
        { "test_replication_push_three_tables",  &test_replication_push_three_tables },
        { "test_replication_mixed_ops",          &test_replication_mixed_ops },
        { "test_replication_value_table_singleton_key", &test_replication_value_table_singleton_key },
        { "test_replication_key_value_bulk_roundtrip", &test_replication_key_value_bulk_roundtrip },
        { "test_replication_key_table_range_delete_roundtrip",
          &test_replication_key_table_range_delete_roundtrip },
        { "test_replication_incremental_pull",   &test_replication_incremental_pull },
        { "test_replication_idempotent_apply",   &test_replication_idempotent_apply },
        { "test_replication_make_push_request", &test_replication_make_push_request_helper },
    };
    int rc = 0;
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        try {
            cases[i].fn();
            std::printf("PASS %s\n", cases[i].name);
        } catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", cases[i].name, e.what());
            rc = static_cast<int>(i + 1);
        } catch (...) {
            std::printf("FAIL %s: non-std exception\n", cases[i].name);
            rc = static_cast<int>(i + 1);
        }
    }
    return rc;
}
