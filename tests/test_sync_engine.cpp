#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

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

template<class KVT>
typename KVT::value_type::second_type kv_or_throw(const std::shared_ptr<mdbxc::Connection>& conn,
        KVT& kv, const typename KVT::value_type::first_type& key, const char* what) {
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

void assign_bytes(std::vector<std::uint8_t>& out, const MDBX_val& val) {
    out.clear();
    if (val.iov_len == 0) {
        return;
    }
    const std::uint8_t* begin = static_cast<const std::uint8_t*>(val.iov_base);
    out.assign(begin, begin + val.iov_len);
}

mdbxc::sync::ChangeBatch make_raw_batch(const mdbxc::sync::NodeId& origin,
                                        std::uint64_t seq,
                                        const std::string& dbi_name,
                                        std::uint8_t key_seed) {
    mdbxc::sync::ChangeBatch batch;
    batch.origin_node_id = origin;
    batch.seq = seq;
    mdbxc::sync::ChangeOp op;
    op.op_type = mdbxc::sync::ChangeOpType::Put;
    op.dbi_name = dbi_name;
    op.storage_key = { key_seed, static_cast<std::uint8_t>(seq & 0xff) };
    op.value = { static_cast<std::uint8_t>(0x80u | key_seed),
                 static_cast<std::uint8_t>(seq & 0xff) };
    batch.ops.push_back(op);
    return batch;
}

void append_raw_batch(mdbxc::sync::ChangeLogStore& log,
                      MDBX_txn* txn,
                      const mdbxc::sync::NodeId& origin,
                      std::uint64_t seq,
                      const std::string& dbi_name,
                      std::uint8_t key_seed) {
    const mdbxc::sync::ChangeBatch batch = make_raw_batch(origin, seq, dbi_name, key_seed);
    const std::vector<std::uint8_t> bytes = mdbxc::sync::ChangeBatchCodec::encode(batch);
    log.append(txn, origin, seq, bytes);
}

void append_raw_bytes(mdbxc::sync::ChangeLogStore& log,
                      MDBX_txn* txn,
                      const mdbxc::sync::NodeId& origin,
                      std::uint64_t seq,
                      const std::vector<std::uint8_t>& bytes) {
    log.append(txn, origin, seq, bytes);
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
    if (kv_or_throw(replica_conn, replica_kv, 1, "replica kv[1]") != 100) throw std::runtime_error("replica kv[1] != 100");
    if (kv_or_throw(replica_conn, replica_kv, 2, "replica kv[2]") != 200) throw std::runtime_error("replica kv[2] != 200");
    if (kv_or_throw(replica_conn, replica_kv, 3, "replica kv[3]") != 300) throw std::runtime_error("replica kv[3] != 300");

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

void test_engine_applies_legacy_zero_flags_to_integer_dbi() {
    using namespace mdbxc;
    const std::string p = "test_engine_legacy_zero_flags.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0x10), make_node(0xD0));
    KeyValueTable<int, int> kv(conn, "kv");

    const int key = 42;
    const int value = 77;
    SerializeScratch key_scratch;
    SerializeScratch value_scratch;
    const MDBX_val db_key = serialize_key<true>(key, key_scratch);
    const MDBX_val db_value = serialize_value(value, value_scratch);

    sync::ChangeBatch batch;
    batch.origin_node_id = make_node(0x20);
    batch.seq = 1;
    sync::ChangeOp op;
    op.op_type = sync::ChangeOpType::Put;
    op.dbi_flags = 0;
    op.dbi_name = "kv";
    assign_bytes(op.storage_key, db_key);
    assign_bytes(op.value, db_value);
    batch.ops.push_back(op);

    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        if (engine.apply_batch(txn.handle(), batch) != sync::ApplyResult::Applied) {
            throw std::runtime_error("legacy zero-flags batch should apply");
        }
        txn.commit();
    }

    if (kv_or_throw(conn, kv, key, "legacy zero-flags kv") != value) {
        throw std::runtime_error("legacy zero-flags kv value mismatch");
    }

    conn->disconnect();
    cleanup(p);
}

void test_engine_conflicting_dbi_flags_returns_conflict() {
    using namespace mdbxc;
    const std::string p = "test_engine_conflicting_dbi_flags.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0x10), make_node(0xD0));

    sync::ChangeBatch batch;
    batch.origin_node_id = make_node(0x20);
    batch.seq = 1;

    sync::ChangeOp first;
    first.op_type = sync::ChangeOpType::Put;
    first.dbi_name = "kv";
    first.dbi_flags = static_cast<std::uint32_t>(MDBX_INTEGERKEY);
    first.storage_key = { 0x01 };
    first.value = { 0x11 };
    batch.ops.push_back(first);

    sync::ChangeOp second = first;
    second.dbi_flags = static_cast<std::uint32_t>(MDBX_REVERSEKEY);
    second.storage_key = { 0x02 };
    second.value = { 0x22 };
    batch.ops.push_back(second);

    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        const sync::ApplyOutcome outcome = engine.apply_batch_ex(txn.handle(), batch);
        if (outcome.result != sync::ApplyResult::Conflict) {
            throw std::runtime_error("conflicting dbi_flags batch should return Conflict");
        }
        if (outcome.conflict_reason != sync::ApplyConflictReason::InconsistentBatchDbiFlags) {
            throw std::runtime_error("conflicting dbi_flags batch returned wrong reason");
        }
        if (outcome.dbi_name != "kv") {
            throw std::runtime_error("conflicting dbi_flags batch returned wrong DBI name");
        }
        txn.commit();
    }

    if (engine.applied_cursor().last_seq_for(make_node(0x20)) != 0u) {
        throw std::runtime_error("conflicting dbi_flags batch advanced cursor");
    }

    conn->disconnect();
    cleanup(p);
}

void test_engine_existing_dbi_flag_mismatch_returns_conflict() {
    using namespace mdbxc;
    const std::string p = "test_engine_existing_dbi_flag_mismatch.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    seed_node_id(conn, make_node(0x10));
    sync::SyncEngine engine(conn);
    KeyValueTable<int, int> kv(conn, "kv");

    const sync::NodeId origin = make_node(0x20);

    SerializeScratch key_scratch;
    SerializeScratch value_scratch;
    const MDBX_val good_key = serialize_key<true>(1, key_scratch);
    const MDBX_val good_value = serialize_value(100, value_scratch);

    sync::ChangeBatch good;
    good.origin_node_id = origin;
    good.seq = 1;
    sync::ChangeOp good_op;
    good_op.op_type = sync::ChangeOpType::Put;
    good_op.dbi_name = "kv";
    good_op.dbi_flags = static_cast<std::uint32_t>(MDBX_INTEGERKEY);
    assign_bytes(good_op.storage_key, good_key);
    assign_bytes(good_op.value, good_value);
    good.ops.push_back(good_op);

    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        if (engine.apply_batch(txn.handle(), good) != sync::ApplyResult::Applied) {
            throw std::runtime_error("matching DBI flags batch should apply");
        }
        txn.commit();
    }

    key_scratch.clear();
    value_scratch.clear();
    const MDBX_val bad_key = serialize_key<true>(2, key_scratch);
    const MDBX_val bad_value = serialize_value(200, value_scratch);

    sync::ChangeBatch bad;
    bad.origin_node_id = origin;
    bad.seq = 2;
    sync::ChangeOp bad_op;
    bad_op.op_type = sync::ChangeOpType::Put;
    bad_op.dbi_name = "kv";
    bad_op.dbi_flags = static_cast<std::uint32_t>(MDBX_REVERSEKEY);
    assign_bytes(bad_op.storage_key, bad_key);
    assign_bytes(bad_op.value, bad_value);
    bad.ops.push_back(bad_op);

    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        const sync::ApplyOutcome outcome = engine.apply_batch_ex(txn.handle(), bad);
        if (outcome.result != sync::ApplyResult::Conflict) {
            throw std::runtime_error("existing DBI flag mismatch should return Conflict");
        }
        if (outcome.conflict_reason != sync::ApplyConflictReason::ExistingDbiFlagsMismatch) {
            throw std::runtime_error("existing DBI flag mismatch returned wrong reason");
        }
        if (outcome.dbi_name != "kv") {
            throw std::runtime_error("existing DBI flag mismatch returned wrong DBI name");
        }
        if (outcome.incoming_dbi_flags != static_cast<std::uint32_t>(MDBX_REVERSEKEY)) {
            throw std::runtime_error("existing DBI flag mismatch returned wrong flags");
        }
        if (!outcome.actual_dbi_flags_available ||
            outcome.actual_dbi_flags != static_cast<std::uint32_t>(MDBX_INTEGERKEY)) {
            throw std::runtime_error("existing DBI flag mismatch returned wrong actual flags");
        }
        txn.commit();
    }

    if (engine.applied_cursor().last_seq_for(origin) != 1u) {
        throw std::runtime_error("existing DBI flag mismatch advanced cursor");
    }
    if (kv_has(conn, kv, 2)) {
        throw std::runtime_error("existing DBI flag mismatch applied data");
    }

    conn->disconnect();
    cleanup(p);
}

void test_engine_existing_dbi_flag_mismatch_reports_first_batch_dbi() {
    using namespace mdbxc;
    const std::string p = "test_engine_existing_dbi_flag_mismatch_first.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    seed_node_id(conn, make_node(0x10));
    sync::SyncEngine engine(conn);
    KeyValueTable<int, int> beta(conn, "beta");
    KeyValueTable<int, int> alpha(conn, "alpha");
    (void)beta;
    (void)alpha;

    SerializeScratch key_scratch;
    SerializeScratch value_scratch;
    const MDBX_val key = serialize_key<true>(1, key_scratch);
    const MDBX_val value = serialize_value(100, value_scratch);

    sync::ChangeBatch batch;
    batch.origin_node_id = make_node(0x20);
    batch.seq = 1;

    sync::ChangeOp beta_op;
    beta_op.op_type = sync::ChangeOpType::Put;
    beta_op.dbi_name = "beta";
    beta_op.dbi_flags = static_cast<std::uint32_t>(MDBX_REVERSEKEY);
    assign_bytes(beta_op.storage_key, key);
    assign_bytes(beta_op.value, value);
    batch.ops.push_back(beta_op);

    sync::ChangeOp alpha_op = beta_op;
    alpha_op.dbi_name = "alpha";
    batch.ops.push_back(alpha_op);

    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        const sync::ApplyOutcome outcome = engine.apply_batch_ex(txn.handle(), batch);
        if (outcome.result != sync::ApplyResult::Conflict) {
            throw std::runtime_error("multi-DBI mismatch should return Conflict");
        }
        if (outcome.conflict_reason != sync::ApplyConflictReason::ExistingDbiFlagsMismatch) {
            throw std::runtime_error("multi-DBI mismatch returned wrong reason");
        }
        if (outcome.dbi_name != "beta") {
            throw std::runtime_error("multi-DBI mismatch did not report first batch DBI");
        }
        if (!outcome.actual_dbi_flags_available ||
            outcome.actual_dbi_flags != static_cast<std::uint32_t>(MDBX_INTEGERKEY)) {
            throw std::runtime_error("multi-DBI mismatch returned wrong actual flags");
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
    const sync::ApplyOutcome gap = engine.apply_batch_ex(txn.handle(), make_batch(3));
    if (gap.result != sync::ApplyResult::Conflict) {
        throw std::runtime_error("seq=3 should be Conflict (gap after seq=1)");
    }
    if (gap.conflict_reason != sync::ApplyConflictReason::SequenceGap ||
        gap.last_applied_seq != 1u ||
        gap.batch_seq != 3u) {
        throw std::runtime_error("seq=3 returned wrong conflict details");
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
    if (kv_or_throw(remote_conn, remote_kv, 7, "remote kv[7]") != 0x77) {
        throw std::runtime_error("remote kv[7] != 0x77 after push");
    }

    origin_conn->disconnect();
    remote_conn->disconnect();
    cleanup(origin_path);
    cleanup(remote_path);
}

void test_engine_push_gap_rolls_back() {
    using namespace mdbxc;
    const std::string p = "test_engine_push_gap.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0x10), make_node(0xD0));

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

    // Direct apply: seq=1 first
    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        if (engine.apply_batch(txn.handle(), make_batch(1)) != sync::ApplyResult::Applied) {
            throw std::runtime_error("seq=1 should apply");
        }
        txn.commit();
    }

    // Push [seq=3] — should be Conflict, ok=false, no commit
    {
        sync::DirectSyncPeer peer(&engine);
        sync::PushRequest req;
        req.sender = make_node(0x20);
        req.db_id  = make_node(0xD0);
        req.batches.push_back(make_batch(3));

        const sync::PushResponse resp = peer.push(req);
        if (resp.ok) {
            throw std::runtime_error("push with gap should return ok=false");
        }
        if (resp.error.find("sequence_gap") == std::string::npos) {
            throw std::runtime_error("push with gap should return sequence_gap error");
        }
    }

    // After rejected push, table 't' should still be empty (rollback worked)
    {
        KeyValueTable<std::uint8_t, std::uint8_t> t(conn, "t");
        if (kv_has(conn, t, static_cast<std::uint8_t>(3))) {
            throw std::runtime_error("seq=3 must not be persisted on rejected push");
        }
    }

    // Receiver cursor should remain at seq=1
    {
        const sync::SyncCursor cur = engine.applied_cursor();
        if (cur.last_seq_for(make_node(0x20)) != 1u) {
            throw std::runtime_error("cursor should still be at seq=1 after rejected push");
        }
    }

    conn->disconnect();
    cleanup(p);
}

void test_engine_handle_pull_wrong_db_id() {
    using namespace mdbxc;
    const std::string p = "test_engine_pull_wrong_db.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xA0), make_node(0xD0));

    sync::DirectSyncPeer peer(&engine);
    sync::PullRequest req;
    req.requester = make_node(0xB0);
    req.db_id     = make_node(0xFF);  // wrong db_id

    const sync::PullResponse resp = peer.pull(req);
    if (resp.ok) {
        throw std::runtime_error("pull with wrong db_id should return ok=false");
    }
    if (!resp.batches.empty()) {
        throw std::runtime_error("pull with wrong db_id should not return batches");
    }

    conn->disconnect();
    cleanup(p);
}

void test_engine_handle_push_wrong_db_id() {
    using namespace mdbxc;
    const std::string p = "test_engine_push_wrong_db.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xA0), make_node(0xD0));

    sync::ChangeBatch b;
    b.origin_node_id = make_node(0x20);
    b.seq = 1;
    sync::ChangeOp op;
    op.op_type = sync::ChangeOpType::Put;
    op.dbi_name = "t";
    op.storage_key = { 0x01 };
    op.value = { 0xAA };
    b.ops.push_back(op);

    sync::DirectSyncPeer peer(&engine);
    sync::PushRequest req;
    req.sender = make_node(0x20);
    req.db_id  = make_node(0xFF);  // wrong
    req.batches.push_back(b);

    const sync::PushResponse resp = peer.push(req);
    if (resp.ok) {
        throw std::runtime_error("push with wrong db_id should return ok=false");
    }

    {
        KeyValueTable<std::uint8_t, std::uint8_t> t(conn, "t");
        if (kv_has(conn, t, static_cast<std::uint8_t>(1))) {
            throw std::runtime_error("table must be empty on wrong db_id push");
        }
    }

    conn->disconnect();
    cleanup(p);
}

void test_engine_handle_pull_pagination_has_more() {
    using namespace mdbxc;
    const std::string primary_path = "test_engine_pull_pag.mdbx";
    const std::string replica_path = "test_engine_pull_pag_replica.mdbx";
    cleanup(primary_path); cleanup(replica_path);

    auto primary_conn = open_env(primary_path);
    auto replica_conn = open_env(replica_path);

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
        for (int i = 1; i <= 5; ++i) {
            kv.insert_or_assign(i, i * 100);
        }
    }
    primary_conn->detach_sync_capture();

    sync::DirectSyncPeer peer(&primary_engine);
    sync::PullRequest req;
    req.requester  = replica_node;
    req.db_id      = db_uuid;
    req.max_batches = 2;  // force pagination
    const sync::PullResponse resp = peer.pull(req);

    if (resp.batches.size() != 2u) {
        throw std::runtime_error("expected 2 batches, got " +
                                 std::to_string(resp.batches.size()));
    }
    if (!resp.has_more) {
        throw std::runtime_error("has_more should be true when limit truncates pull");
    }

    // Apply first page
    {
        auto txn = replica_conn->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& b : resp.batches) {
            replica_engine.apply_batch(txn.handle(), b);
        }
        txn.commit();
    }

    // Pull page 2 with cursor from page 1
    sync::PullRequest req2;
    req2.requester = replica_node;
    req2.db_id     = db_uuid;
    req2.have      = replica_engine.applied_cursor();
    req2.max_batches = 100;  // no limit this time
    const sync::PullResponse resp2 = peer.pull(req2);
    if (resp2.batches.size() != 3u) {
        throw std::runtime_error("expected 3 remaining batches, got " +
                                 std::to_string(resp2.batches.size()));
    }
    if (resp2.has_more) {
        throw std::runtime_error("has_more should be false after draining changelog");
    }

    {
        auto txn = replica_conn->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& b : resp2.batches) {
            replica_engine.apply_batch(txn.handle(), b);
        }
        txn.commit();
    }

    KeyValueTable<int, int> replica_kv(replica_conn, "kv");
    for (int i = 1; i <= 5; ++i) {
        if (kv_or_throw(replica_conn, replica_kv, i, "missing on replica") != i * 100) {
            throw std::runtime_error("wrong value on replica after paginated pull");
        }
    }

    primary_conn->disconnect();
    replica_conn->disconnect();
    cleanup(primary_path); cleanup(replica_path);
}

void test_engine_handle_pull_multi_origin_pagination() {
    using namespace mdbxc;
    const std::string primary_path = "test_engine_pull_multi_origin.mdbx";
    const std::string replica_path = "test_engine_pull_multi_origin_replica.mdbx";
    cleanup(primary_path); cleanup(replica_path);

    auto primary_conn = open_env(primary_path);
    auto replica_conn = open_env(replica_path);

    const sync::NodeId primary_node = make_node(0xA0);
    const sync::NodeId replica_node = make_node(0xB0);
    const sync::NodeId db_uuid = make_node(0xD0);
    const sync::NodeId origin_a = make_node(0x20);
    const sync::NodeId origin_b = make_node(0x40);

    sync::SyncEngine primary_engine(primary_conn);
    sync::SyncEngine replica_engine(replica_conn);
    primary_engine.initialize_local_identity(primary_node, db_uuid);
    replica_engine.initialize_local_identity(replica_node, db_uuid);

    {
        auto txn = primary_conn->transaction(TransactionMode::WRITABLE);
        sync::ChangeLogStore log(primary_conn->env_handle());
        log.open(txn.handle());
        append_raw_batch(log, txn.handle(), origin_a, 1, "kv", 0xA1);
        append_raw_batch(log, txn.handle(), origin_a, 2, "kv", 0xA2);
        append_raw_batch(log, txn.handle(), origin_b, 1, "kv", 0xB1);
        append_raw_batch(log, txn.handle(), origin_b, 2, "kv", 0xB2);
        txn.commit();
    }

    sync::DirectSyncPeer peer(&primary_engine);
    sync::PullRequest req;
    req.requester = replica_node;
    req.db_id = db_uuid;
    req.max_batches = 2;
    const sync::PullResponse first = peer.pull(req);
    if (first.batches.size() != 2u || !first.has_more) {
        throw std::runtime_error("first multi-origin page should contain 2 batches and has_more");
    }
    if (first.batches[0].origin_node_id != origin_a ||
        first.batches[1].origin_node_id != origin_a) {
        throw std::runtime_error("first multi-origin page should stop inside origin A");
    }

    {
        auto txn = replica_conn->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& batch : first.batches) {
            if (replica_engine.apply_batch(txn.handle(), batch) != sync::ApplyResult::Applied) {
                throw std::runtime_error("first multi-origin page apply failed");
            }
        }
        txn.commit();
    }

    sync::PullRequest req2;
    req2.requester = replica_node;
    req2.db_id = db_uuid;
    req2.have = replica_engine.applied_cursor();
    req2.max_batches = 2;
    const sync::PullResponse second = peer.pull(req2);
    if (second.batches.size() != 2u) {
        throw std::runtime_error("second multi-origin page should contain origin B batches");
    }
    if (second.has_more) {
        throw std::runtime_error("second multi-origin page should drain changelog");
    }
    if (second.batches[0].origin_node_id != origin_b ||
        second.batches[1].origin_node_id != origin_b) {
        throw std::runtime_error("second multi-origin page should include origin B");
    }

    {
        auto txn = replica_conn->transaction(TransactionMode::WRITABLE);
        for (const sync::ChangeBatch& batch : second.batches) {
            if (replica_engine.apply_batch(txn.handle(), batch) != sync::ApplyResult::Applied) {
                throw std::runtime_error("second multi-origin page apply failed");
            }
        }
        txn.commit();
    }

    const sync::SyncCursor cursor = replica_engine.applied_cursor();
    if (cursor.last_seq_for(origin_a) != 2u || cursor.last_seq_for(origin_b) != 2u) {
        throw std::runtime_error("multi-origin pagination did not apply both origins");
    }

    primary_conn->disconnect();
    replica_conn->disconnect();
    cleanup(primary_path); cleanup(replica_path);
}

void test_engine_handle_pull_skips_old_batches_without_decoding() {
    using namespace mdbxc;
    const std::string primary_path = "test_engine_pull_skip_old_decode.mdbx";
    cleanup(primary_path);

    auto primary_conn = open_env(primary_path);

    const sync::NodeId primary_node = make_node(0xA0);
    const sync::NodeId replica_node = make_node(0xB0);
    const sync::NodeId db_uuid = make_node(0xD0);
    const sync::NodeId origin_a = make_node(0x20);
    const sync::NodeId origin_b = make_node(0x40);

    sync::SyncEngine primary_engine(primary_conn);
    primary_engine.initialize_local_identity(primary_node, db_uuid);

    {
        auto txn = primary_conn->transaction(TransactionMode::WRITABLE);
        sync::ChangeLogStore log(primary_conn->env_handle());
        log.open(txn.handle());
        append_raw_bytes(log, txn.handle(), origin_a, 1, std::vector<std::uint8_t>{ 0x01, 0x02 });
        append_raw_batch(log, txn.handle(), origin_b, 1, "kv", 0xB1);
        txn.commit();
    }

    sync::DirectSyncPeer peer(&primary_engine);
    sync::PullRequest req;
    req.requester = replica_node;
    req.db_id = db_uuid;
    req.have.last_seq_by_origin[origin_a] = 1;

    const sync::PullResponse response = peer.pull(req);
    if (!response.ok) {
        throw std::runtime_error("skip-old pull should succeed");
    }
    if (response.batches.size() != 1u) {
        throw std::runtime_error("skip-old pull should return only origin B batch");
    }
    if (response.batches[0].origin_node_id != origin_b ||
        response.batches[0].seq != 1u) {
        throw std::runtime_error("skip-old pull returned the wrong batch");
    }

    primary_conn->disconnect();
    cleanup(primary_path);
}

void test_engine_handle_pull_lifecycle() {
    using namespace mdbxc;
    const std::string p = "test_engine_pull_lifecycle.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    sync::SyncEngine local_engine(conn);
    local_engine.initialize_local_identity(make_node(0x10), make_node(0x10));

    sync::DirectSyncPeer peer(&local_engine);
    sync::PullRequest req;
    req.requester = make_node(0x20);
    req.db_id     = make_node(0x10);
    // Many handle_pull() calls in sequence — the read txn guard must abort
    // (release handle + reader slot) every time, otherwise long-lived
    // servers would slowly exhaust the reader table (MDBX_READERS limit).
    // This test will catch a regression back to mdbx_txn_reset().
    for (int i = 0; i < 256; ++i) {
        const sync::PullResponse resp = peer.pull(req);
        if (!resp.ok) {
            throw std::runtime_error("pull lifecycle: ok=false at i=" +
                                     std::to_string(i));
        }
    }

    conn->disconnect();
    cleanup(p);
}

} // namespace

int main() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "test_engine_round_trip_kv",          &test_engine_round_trip_kv },
        { "test_engine_skips_self_origin",      &test_engine_skips_self_origin },
        { "test_engine_idempotent_replay",      &test_engine_idempotent_replay },
        { "test_engine_legacy_zero_flags",      &test_engine_applies_legacy_zero_flags_to_integer_dbi },
        { "test_engine_conflicting_dbi_flags",  &test_engine_conflicting_dbi_flags_returns_conflict },
        { "test_engine_existing_dbi_flag_mismatch",&test_engine_existing_dbi_flag_mismatch_returns_conflict },
        { "test_engine_existing_dbi_flag_mismatch_first",&test_engine_existing_dbi_flag_mismatch_reports_first_batch_dbi },
        { "test_engine_gap_returns_conflict",   &test_engine_gap_returns_conflict },
        { "test_engine_applied_cursor",         &test_engine_applied_cursor },
        { "test_engine_handle_push_to_remote",  &test_engine_handle_push_to_remote },
        { "test_engine_push_gap_rolls_back",    &test_engine_push_gap_rolls_back },
        { "test_engine_handle_pull_wrong_db_id",&test_engine_handle_pull_wrong_db_id },
        { "test_engine_handle_push_wrong_db_id",&test_engine_handle_push_wrong_db_id },
        { "test_engine_handle_pull_pagination", &test_engine_handle_pull_pagination_has_more },
        { "test_engine_handle_pull_multi_origin",&test_engine_handle_pull_multi_origin_pagination },
        { "test_engine_handle_pull_skip_old_decode",&test_engine_handle_pull_skips_old_batches_without_decoding },
        { "test_engine_handle_pull_lifecycle", &test_engine_handle_pull_lifecycle },
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
