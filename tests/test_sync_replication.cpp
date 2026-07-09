/// \file test_sync_replication.cpp
/// \brief Multi-table replication scenarios for the sync engine.

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        { "test_replication_incremental_pull",   &test_replication_incremental_pull },
        { "test_replication_idempotent_apply",   &test_replication_idempotent_apply },
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