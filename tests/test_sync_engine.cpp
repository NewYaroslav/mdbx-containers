#include <mdbx_containers.hpp>
#include <mdbx_containers/Sync.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void cleanup(const std::string& p) {
    std::remove(p.c_str());
}

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId n{};
    for (int i = 0; i < 16; ++i) {
        n[i] = static_cast<std::uint8_t>(seed + i);
    }
    return n;
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path) {
    using namespace mdbxc;
    Config cfg;
    cfg.pathname = path;
    cfg.max_dbs = 16;
    cfg.no_subdir = true;
    return Connection::create(cfg);
}

void seed_node_id(std::shared_ptr<mdbxc::Connection> conn,
                  const mdbxc::sync::NodeId& node_id) {
    using namespace mdbxc;
    sync::MetaStore meta(conn->env_handle());
    auto txn = conn->transaction(TransactionMode::WRITABLE);
    meta.open(txn.handle());
    meta.set_node_id(txn.handle(), node_id);
    txn.commit();
}

void test_engine_round_trip_kv() {
    using namespace mdbxc;
    const std::string primary_path = "test_engine_primary.mdbx";
    const std::string replica_path = "test_engine_replica.mdbx";
    cleanup(primary_path);
    cleanup(replica_path);

    auto primary_conn   = open_env(primary_path);
    auto replica_conn   = open_env(replica_path);

    const sync::NodeId primary_node = make_node(0xA0);
    const sync::NodeId replica_node = make_node(0xB0);
    const sync::NodeId db_uuid      = make_node(0xD0);

    sync::SyncEngine primary_engine(primary_conn);
    sync::SyncEngine replica_engine(replica_conn);
    primary_engine.initialize_local_identity(primary_node, db_uuid);
    replica_engine.initialize_local_identity(replica_node, db_uuid);

    sync::ThreadLocalChangeAccumulator primary_sink(primary_conn);
    primary_conn->attach_sync_capture(&primary_sink);

    {
        KeyValueTable<int, int> kv(primary_conn, "kv");
        kv.insert_or_assign(1, 100);
        kv.insert_or_assign(2, 200);
        kv.insert_or_assign(3, 300);
    }

    primary_conn->detach_sync_capture();

    sync::DirectSyncPeer peer(&primary_engine);
    sync::PullRequest req;
    req.requester = replica_node;
    req.db_id     = db_uuid;
    const sync::PullResponse resp = peer.pull(req);

    if (resp.batches.size() != 3u) {
        throw std::runtime_error("expected 3 batches, got " +
                                 std::to_string(resp.batches.size()));
    }

    {
        auto txn = replica_conn->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& batch : resp.batches) {
            const sync::ApplyResult r = replica_engine.apply_batch(txn.handle(), batch);
            if (r != sync::ApplyResult::Applied) {
                throw std::runtime_error("apply_batch did not return Applied");
            }
        }
        txn.commit();
    }

    KeyValueTable<int, int> replica_kv(replica_conn, "kv");
    auto v1 = replica_kv.find(1);
    auto v2 = replica_kv.find(2);
    auto v3 = replica_kv.find(3);
    if (!v1 || *v1 != 100) throw std::runtime_error("replica kv[1] != 100");
    if (!v2 || *v2 != 200) throw std::runtime_error("replica kv[2] != 200");
    if (!v3 || *v3 != 300) throw std::runtime_error("replica kv[3] != 300");

    replica_conn->disconnect();
    cleanup(primary_path);
    cleanup(replica_path);
}

void test_engine_skips_self_origin() {
    using namespace mdbxc;
    const std::string p = "test_engine_self_origin.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    seed_node_id(conn, make_node(0x10));

    sync::SyncEngine engine(conn);
    sync::MetaStore meta(conn->env_handle());
    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        meta.open(txn.handle());
        meta.set_node_id(txn.handle(), make_node(0x10));
        txn.commit();
    }

    sync::ChangeBatch batch;
    batch.origin_node_id = make_node(0x10);
    batch.seq = 1;
    sync::ChangeOp op;
    op.op_type = sync::ChangeOpType::Put;
    op.dbi_name = "t";
    op.storage_key = { 0x01 };
    op.value = { 0xAA };
    batch.ops.push_back(op);

    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        const sync::ApplyResult r = engine.apply_batch(txn.handle(), batch);
        if (r != sync::ApplyResult::Skipped) {
            throw std::runtime_error("self-origin batch should be Skipped");
        }
        txn.commit();
    }

    conn->disconnect();
    cleanup(p);
}

void test_engine_idempotent_replay() {
    using namespace mdbxc;
    const std::string p = "test_engine_replay.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    seed_node_id(conn, make_node(0x10));

    sync::SyncEngine engine(conn);

    sync::ChangeBatch batch;
    batch.origin_node_id = make_node(0x20);
    batch.seq = 1;
    sync::ChangeOp op;
    op.op_type = sync::ChangeOpType::Put;
    op.dbi_name = "t";
    op.storage_key = { 0x42 };
    op.value = { 0x11, 0x22 };
    batch.ops.push_back(op);

    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        if (engine.apply_batch(txn.handle(), batch) != sync::ApplyResult::Applied) {
            throw std::runtime_error("first apply should be Applied");
        }
        if (engine.apply_batch(txn.handle(), batch) != sync::ApplyResult::Skipped) {
            throw std::runtime_error("second apply should be Skipped");
        }
        txn.commit();
    }

    conn->disconnect();
    cleanup(p);
}

void test_engine_gap_returns_conflict() {
    using namespace mdbxc;
    const std::string p = "test_engine_gap.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    seed_node_id(conn, make_node(0x10));
    sync::SyncEngine engine(conn);

    auto make_batch = [](std::uint64_t seq) {
        sync::ChangeBatch b;
        b.origin_node_id = make_node(0x20);
        b.seq = seq;
        sync::ChangeOp op;
        op.op_type = sync::ChangeOpType::Put;
        op.dbi_name = "t";
        op.storage_key = { static_cast<std::uint8_t>(seq) };
        op.value = { 0xFF };
        b.ops.push_back(op);
        return b;
    };

    auto txn = conn->transaction(TransactionMode::WRITABLE);
    if (engine.apply_batch(txn.handle(), make_batch(1)) != sync::ApplyResult::Applied) {
        throw std::runtime_error("seq=1 should apply");
    }
    if (engine.apply_batch(txn.handle(), make_batch(3)) != sync::ApplyResult::Conflict) {
        throw std::runtime_error("seq=3 should be Conflict (gap after seq=1)");
    }
    if (engine.apply_batch(txn.handle(), make_batch(2)) != sync::ApplyResult::Applied) {
        throw std::runtime_error("seq=2 should apply after seq=1");
    }
    txn.commit();

    conn->disconnect();
    cleanup(p);
}

void test_engine_applied_cursor() {
    using namespace mdbxc;
    const std::string p = "test_engine_cursor.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    seed_node_id(conn, make_node(0x10));
    sync::SyncEngine engine(conn);

    auto make_batch = [](std::uint64_t seq) {
        sync::ChangeBatch b;
        b.origin_node_id = make_node(0x20);
        b.seq = seq;
        sync::ChangeOp op;
        op.op_type = sync::ChangeOpType::Put;
        op.dbi_name = "t";
        op.storage_key = { static_cast<std::uint8_t>(seq) };
        op.value = { 0xFF };
        b.ops.push_back(op);
        return b;
    };

    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        engine.apply_batch(txn.handle(), make_batch(1));
        engine.apply_batch(txn.handle(), make_batch(2));
        txn.commit();
    }

    const sync::SyncCursor cur = engine.applied_cursor();
    const std::uint64_t last =
        cur.last_seq_for(make_node(0x20));
    if (last != 2u) {
        throw std::runtime_error("cursor should report last=2, got " +
                                 std::to_string(last));
    }

    conn->disconnect();
    cleanup(p);
}

void test_engine_handle_push_to_remote() {
    using namespace mdbxc;
    const std::string origin_path = "test_engine_push_origin.mdbx";
    const std::string remote_path = "test_engine_push_remote.mdbx";
    cleanup(origin_path);
    cleanup(remote_path);

    auto origin_conn = open_env(origin_path);
    auto remote_conn = open_env(remote_path);

    sync::SyncEngine origin_engine(origin_conn);
    sync::SyncEngine remote_engine(remote_conn);
    origin_engine.initialize_local_identity(make_node(0xA0), make_node(0xD0));
    remote_engine.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    sync::ThreadLocalChangeAccumulator origin_sink(origin_conn);
    origin_conn->attach_sync_capture(&origin_sink);
    {
        KeyValueTable<int, int> kv(origin_conn, "kv");
        kv.insert_or_assign(7, 0x77);
    }
    origin_conn->detach_sync_capture();

    sync::DirectSyncPeer peer(&remote_engine);
    sync::PushRequest req;
    req.sender = make_node(0xA0);
    req.db_id  = make_node(0xD0);

    {
        auto txn = origin_conn->transaction(TransactionMode::WRITABLE);
        sync::MetaStore meta(origin_conn->env_handle());
        meta.open(txn.handle());
        sync::ChangeLogStore log(origin_conn->env_handle());
        log.open(txn.handle());
        const std::uint64_t last_seq = meta.get_local_seq(txn.handle());
        std::vector<std::uint8_t> buf;
        if (!log.get(txn.handle(), make_node(0xA0), last_seq, buf)) {
            throw std::runtime_error("origin changelog has no batch for last seq");
        }
        const sync::ChangeBatch b = sync::ChangeBatchCodec::decode_exact(buf);
        req.batches.push_back(b);
        txn.commit();
    }

    const sync::PushResponse resp = peer.push(req);
    if (!resp.ok) {
        throw std::runtime_error("push should succeed: " + resp.error);
    }
    if (resp.receiver_have.last_seq_for(make_node(0xA0)) != 1u) {
        throw std::runtime_error("receiver cursor should reflect applied seq");
    }

    KeyValueTable<int, int> remote_kv(remote_conn, "kv");
    auto v = remote_kv.find(7);
    if (!v || *v != 0x77) {
        throw std::runtime_error("remote kv[7] != 0x77 after push");
    }

    origin_conn->disconnect();
    remote_conn->disconnect();
    cleanup(origin_path);
    cleanup(remote_path);
}

} // namespace

int main() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "test_engine_round_trip_kv",          &test_engine_round_trip_kv },
        { "test_engine_skips_self_origin",      &test_engine_skips_self_origin },
        { "test_engine_idempotent_replay",      &test_engine_idempotent_replay },
        { "test_engine_gap_returns_conflict",   &test_engine_gap_returns_conflict },
        { "test_engine_applied_cursor",         &test_engine_applied_cursor },
        { "test_engine_handle_push_to_remote",  &test_engine_handle_push_to_remote },
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