#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void cleanup(const std::string& p) {
    std::remove(p.c_str());
}

class StubSink : public mdbxc::sync::ISyncCaptureSink {
public:
    mutable std::mutex m_mutex;
    std::vector<mdbxc::sync::ChangeOp> m_recorded;
    std::vector<mdbxc::sync::ChangeBatch> m_flushed;

    void record_change(MDBX_txn* txn,
                       const std::string& dbi_name,
                       mdbxc::sync::ChangeOpType op_type,
                       std::uint32_t dbi_flags,
                       const std::vector<std::uint8_t>& storage_key,
                       const std::vector<std::uint8_t>& value) override {
        (void)txn;
        std::lock_guard<std::mutex> lk(m_mutex);
        mdbxc::sync::ChangeOp op;
        op.op_type = op_type;
        op.dbi_flags = dbi_flags;
        op.dbi_name = dbi_name;
        op.storage_key = storage_key;
        op.value = value;
        m_recorded.push_back(std::move(op));
    }

    void flush_in_txn(MDBX_txn* txn) override {
        (void)txn;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_flushed.emplace_back();
    }

    void clear_recorded() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_recorded.clear();
        m_flushed.clear();
    }
};

mdbxc::Embedding make_embedding(const std::vector<float>& values) {
    mdbxc::Embedding e;
    e.dim = static_cast<std::uint32_t>(values.size());
    e.values = values;
    return e;
}

void test_no_sink_no_capture() {
    using namespace mdbxc;
    const std::string p = "test_capture_no_sink.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    KeyValueTable<int, int> kv(conn, "t");
    kv.insert_or_assign(1, 100);

    cleanup(p);
}

void test_capture_writes_via_sink() {
    using namespace mdbxc;
    const std::string p = "test_capture_with_sink.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    conn->attach_sync_capture(&sink);

    KeyValueTable<int, int> kv(conn, "t");
    kv.insert_or_assign(1, 100);
    kv.insert_or_assign(2, 200);
    kv.insert_or_assign(3, 300);
    kv.erase(2);

    conn->detach_sync_capture();
    cleanup(p);

    if (sink.m_recorded.size() != 4u) {
        throw std::runtime_error("expected 4 recorded ops, got " +
                                 std::to_string(sink.m_recorded.size()));
    }
    if (sink.m_flushed.size() < 1u) {
        throw std::runtime_error("expected at least one flush_in_txn call");
    }
    if (sink.m_recorded[0].op_type != sync::ChangeOpType::Put ||
        sink.m_recorded[1].op_type != sync::ChangeOpType::Put ||
        sink.m_recorded[2].op_type != sync::ChangeOpType::Put ||
        sink.m_recorded[3].op_type != sync::ChangeOpType::Delete) {
        throw std::runtime_error("op_type sequence incorrect");
    }
    if (sink.m_recorded[0].dbi_name != "t") {
        throw std::runtime_error("dbi_name not propagated");
    }
    if ((sink.m_recorded[0].dbi_flags & MDBX_INTEGERKEY) == 0) {
        throw std::runtime_error("dbi_flags did not propagate MDBX_INTEGERKEY");
    }
}

void test_capture_scope_restores_previous_sink() {
    using namespace mdbxc;
    const std::string p = "test_capture_scope.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink outer_sink;
    StubSink inner_sink;
    conn->attach_sync_capture(&outer_sink);
    if (conn->sync_capture() != &outer_sink) {
        throw std::runtime_error("outer sink was not attached");
    }

    KeyValueTable<int, int> kv(conn, "t");
    {
        sync::SyncCaptureScope scope(conn, inner_sink);
        if (!scope.active() || conn->sync_capture() != &inner_sink) {
            throw std::runtime_error("capture scope did not attach inner sink");
        }
        kv.insert_or_assign(1, 100);
    }

    if (conn->sync_capture() != &outer_sink) {
        throw std::runtime_error("capture scope did not restore outer sink");
    }
    kv.insert_or_assign(2, 200);

    conn->detach_sync_capture();
    kv.insert_or_assign(3, 300);
    conn->disconnect();
    cleanup(p);

    if (inner_sink.m_recorded.size() != 1u ||
        inner_sink.m_recorded[0].op_type != sync::ChangeOpType::Put) {
        throw std::runtime_error("inner sink did not capture scoped write");
    }
    if (outer_sink.m_recorded.size() != 1u ||
        outer_sink.m_recorded[0].op_type != sync::ChangeOpType::Put) {
        throw std::runtime_error("outer sink did not capture restored write");
    }
}

void test_capture_scope_nested_scopes_restore_in_order() {
    using namespace mdbxc;
    const std::string p = "test_capture_scope_nested.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink outer_sink;
    StubSink inner_sink;
    KeyValueTable<int, int> kv(conn, "t");

    {
        sync::SyncCaptureScope outer(conn, outer_sink);
        if (conn->sync_capture() != &outer_sink) {
            throw std::runtime_error("outer scope did not attach outer sink");
        }
        kv.insert_or_assign(1, 100);

        {
            sync::SyncCaptureScope inner(conn, inner_sink);
            if (conn->sync_capture() != &inner_sink) {
                throw std::runtime_error("inner scope did not attach inner sink");
            }
            kv.insert_or_assign(2, 200);
        }

        if (conn->sync_capture() != &outer_sink) {
            throw std::runtime_error("inner scope did not restore outer sink");
        }
        kv.insert_or_assign(3, 300);
    }

    if (conn->sync_capture() != nullptr) {
        throw std::runtime_error("outer scope did not restore null sink");
    }
    kv.insert_or_assign(4, 400);
    conn->disconnect();
    cleanup(p);

    if (outer_sink.m_recorded.size() != 2u) {
        throw std::runtime_error("outer sink captured wrong nested count");
    }
    if (inner_sink.m_recorded.size() != 1u) {
        throw std::runtime_error("inner sink captured wrong nested count");
    }
}

void test_capture_scope_rejects_out_of_order_detach() {
    using namespace mdbxc;
    const std::string p = "test_capture_scope_out_of_order_detach.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink outer_sink;
    StubSink inner_sink;
    KeyValueTable<int, int> kv(conn, "t");

    {
        sync::SyncCaptureScope outer(conn, outer_sink);
        {
            sync::SyncCaptureScope inner(conn, inner_sink);
            bool rejected = false;
            try {
                outer.detach();
            } catch (const std::logic_error&) {
                rejected = true;
            }
            if (!rejected) {
                throw std::runtime_error("out-of-order detach was not rejected");
            }
            if (!outer.active() || conn->sync_capture() != &inner_sink) {
                throw std::runtime_error("out-of-order detach changed active inner sink");
            }
            kv.insert_or_assign(1, 100);
        }
        if (conn->sync_capture() != &outer_sink) {
            throw std::runtime_error("inner scope did not restore outer after rejected detach");
        }
        kv.insert_or_assign(2, 200);
    }

    if (conn->sync_capture() != nullptr) {
        throw std::runtime_error("outer scope did not restore null after rejected detach");
    }
    kv.insert_or_assign(3, 300);
    conn->disconnect();
    cleanup(p);

    if (inner_sink.m_recorded.size() != 1u) {
        throw std::runtime_error("inner sink captured wrong out-of-order count");
    }
    if (outer_sink.m_recorded.size() != 1u) {
        throw std::runtime_error("outer sink captured wrong out-of-order count");
    }
}

void test_capture_scope_rejects_same_sink_out_of_order_detach() {
    using namespace mdbxc;
    const std::string p = "test_capture_scope_same_sink_out_of_order_detach.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    KeyValueTable<int, int> kv(conn, "t");

    sync::SyncCaptureScope outer(conn, sink);
    {
        sync::SyncCaptureScope inner(conn, sink);
        bool rejected = false;
        try {
            outer.detach();
        } catch (const std::logic_error&) {
            rejected = true;
        }
        if (!rejected) {
            throw std::runtime_error("same-sink out-of-order detach was not rejected");
        }
        if (!outer.active() || conn->sync_capture() != &sink) {
            throw std::runtime_error("same-sink rejected detach changed attachment");
        }
        kv.insert_or_assign(1, 100);
    }

    if (conn->sync_capture() != &sink) {
        throw std::runtime_error("same-sink inner scope did not restore outer token");
    }
    kv.insert_or_assign(2, 200);

    outer.detach();
    if (outer.active() || conn->sync_capture() != nullptr) {
        throw std::runtime_error("same-sink outer scope did not restore null sink");
    }
    kv.insert_or_assign(3, 300);
    conn->disconnect();
    cleanup(p);

    if (sink.m_recorded.size() != 2u) {
        throw std::runtime_error("same-sink nested scopes captured wrong count");
    }
}

void test_capture_scope_detach_restores_null_once() {
    using namespace mdbxc;
    const std::string p = "test_capture_scope_detach.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    KeyValueTable<int, int> kv(conn, "t");

    sync::SyncCaptureScope scope(conn, sink);
    if (!scope.active() || conn->sync_capture() != &sink) {
        throw std::runtime_error("scope did not attach sink before detach");
    }
    kv.insert_or_assign(1, 100);

    scope.detach();
    if (scope.active() || conn->sync_capture() != nullptr) {
        throw std::runtime_error("scope did not restore null sink on detach");
    }
    scope.detach();
    if (conn->sync_capture() != nullptr) {
        throw std::runtime_error("double detach changed null sink");
    }

    kv.insert_or_assign(2, 200);
    conn->disconnect();
    cleanup(p);

    if (sink.m_recorded.size() != 1u) {
        throw std::runtime_error("detached scope captured after detach");
    }
}

void test_capture_scope_rejects_null_arguments() {
    using namespace mdbxc;
    const std::string p = "test_capture_scope_null_args.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    bool null_connection_rejected = false;
    bool null_sink_rejected = false;

    try {
        std::shared_ptr<Connection> null_connection;
        sync::SyncCaptureScope scope(null_connection, sink);
        (void)scope;
    } catch (const std::invalid_argument&) {
        null_connection_rejected = true;
    }

    try {
        sync::SyncCaptureScope scope(conn,
                                     static_cast<sync::ISyncCaptureSink*>(nullptr));
        (void)scope;
    } catch (const std::invalid_argument&) {
        null_sink_rejected = true;
    }

    conn->disconnect();
    cleanup(p);

    if (!null_connection_rejected) {
        throw std::runtime_error("null connection was not rejected");
    }
    if (!null_sink_rejected) {
        throw std::runtime_error("null sink was not rejected");
    }
}

void test_sequence_set_writes_via_sink() {
    using namespace mdbxc;
    const std::string p = "test_capture_sequence_set.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    conn->attach_sync_capture(&sink);

    SequenceTable<std::string> events(conn, "events");
    events.insert_or_assign(7, "seven");
    events.insert_or_assign(8, "");

    conn->detach_sync_capture();
    conn->disconnect();
    cleanup(p);

    if (sink.m_recorded.size() != 2u) {
        throw std::runtime_error("expected two sequence set ops, got " +
                                 std::to_string(sink.m_recorded.size()));
    }
    for (std::size_t i = 0; i < sink.m_recorded.size(); ++i) {
        const sync::ChangeOp& op = sink.m_recorded[i];
        if (op.op_type != sync::ChangeOpType::Put) {
            throw std::runtime_error("sequence set must capture Put");
        }
        if (op.dbi_name != "events") {
            throw std::runtime_error("sequence set dbi_name not propagated");
        }
        if (op.storage_key.empty()) {
            throw std::runtime_error("sequence set key not captured");
        }
        if ((op.dbi_flags & MDBX_INTEGERKEY) == 0) {
            throw std::runtime_error("sequence set dbi_flags missing MDBX_INTEGERKEY");
        }
    }
    if (sink.m_recorded[0].value.empty()) {
        throw std::runtime_error("non-empty sequence set value not captured");
    }
    if (!sink.m_recorded[1].value.empty()) {
        throw std::runtime_error("empty sequence set value must remain empty");
    }
}

void test_value_table_writes_storage_key_via_sink() {
    using namespace mdbxc;
    const std::string p = "test_capture_value_table.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    conn->attach_sync_capture(&sink);

    ValueTable<int> state(conn, "state");
    if (!state.insert(41)) {
        throw std::runtime_error("value insert unexpectedly returned false");
    }
    state.set(42);
    if (!state.erase()) {
        throw std::runtime_error("value erase unexpectedly returned false");
    }

    conn->detach_sync_capture();
    conn->disconnect();
    cleanup(p);

    if (sink.m_recorded.size() != 3u) {
        throw std::runtime_error("expected three value ops, got " +
                                 std::to_string(sink.m_recorded.size()));
    }
    const sync::ChangeOpType expected[] = {
        sync::ChangeOpType::Put,
        sync::ChangeOpType::Put,
        sync::ChangeOpType::Delete
    };
    for (std::size_t i = 0; i < sink.m_recorded.size(); ++i) {
        const sync::ChangeOp& op = sink.m_recorded[i];
        if (op.dbi_name != "state") {
            throw std::runtime_error("value table dbi_name not propagated");
        }
        if (op.op_type != expected[i]) {
            throw std::runtime_error("value table op_type sequence incorrect");
        }
        if (op.storage_key.empty()) {
            throw std::runtime_error("value table storage_key must be physical key bytes");
        }
        if ((op.dbi_flags & MDBX_INTEGERKEY) == 0) {
            throw std::runtime_error("value table dbi_flags missing MDBX_INTEGERKEY");
        }
        if (op.op_type == sync::ChangeOpType::Put && op.value.empty()) {
            throw std::runtime_error("value table Put missing value bytes");
        }
    }
}

void test_key_value_bulk_writes_via_sink() {
    using namespace mdbxc;
    const std::string p = "test_capture_kv_bulk.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    conn->attach_sync_capture(&sink);

    KeyValueTable<int, int> kv(conn, "bulk");
    std::map<int, int> initial;
    initial[1] = 10;
    initial[2] = 20;
    kv.append(initial);

    std::vector<std::pair<int, int> > extra;
    extra.push_back(std::make_pair(3, 30));
    kv.append(extra);

    std::map<int, int> replacement;
    replacement[2] = 200;
    replacement[3] = 300;
    replacement[4] = 400;
    kv.reconcile(replacement);

    std::vector<std::pair<int, int> > final_state;
    final_state.push_back(std::make_pair(4, 440));
    kv.reconcile(final_state);

    conn->detach_sync_capture();
    conn->disconnect();
    cleanup(p);

    if (sink.m_recorded.size() != 10u) {
        throw std::runtime_error("expected ten bulk ops, got " +
                                 std::to_string(sink.m_recorded.size()));
    }
    std::size_t put_count = 0;
    std::size_t delete_count = 0;
    for (const sync::ChangeOp& op : sink.m_recorded) {
        if (op.dbi_name != "bulk") {
            throw std::runtime_error("bulk dbi_name not propagated");
        }
        if (op.storage_key.empty()) {
            throw std::runtime_error("bulk op missing storage_key");
        }
        if (op.op_type == sync::ChangeOpType::Put) {
            ++put_count;
            if (op.value.empty()) {
                throw std::runtime_error("bulk Put missing value bytes");
            }
        } else if (op.op_type == sync::ChangeOpType::Delete) {
            ++delete_count;
        } else {
            throw std::runtime_error("bulk op_type unexpected");
        }
    }
    if (put_count != 7u || delete_count != 3u) {
        throw std::runtime_error("bulk Put/Delete counts incorrect");
    }
}

void test_range_erase_writes_via_sink() {
    using namespace mdbxc;
    const std::string p = "test_capture_range_erase.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    conn->attach_sync_capture(&sink);

    KeyTable<int> keys(conn, "keys");
    keys.insert(1);
    keys.insert(2);
    keys.insert(3);
    keys.insert(4);
    sink.clear_recorded();
    if (keys.erase_range(2, 3) != 2u) {
        throw std::runtime_error("key range erase count incorrect");
    }
    if (sink.m_recorded.size() != 2u) {
        throw std::runtime_error("expected two key range Delete ops");
    }
    for (const sync::ChangeOp& op : sink.m_recorded) {
        if (op.dbi_name != "keys" ||
            op.op_type != sync::ChangeOpType::Delete ||
            op.storage_key.empty()) {
            throw std::runtime_error("key range Delete capture incorrect");
        }
    }

    KeyValueTable<int, int> kv(conn, "range_kv");
    kv.insert_or_assign(1, 10);
    kv.insert_or_assign(2, 20);
    kv.insert_or_assign(3, 30);
    kv.insert_or_assign(4, 40);
    sink.clear_recorded();
    if (kv.erase_range(2, 3) != 2u) {
        throw std::runtime_error("kv range erase count incorrect");
    }
    if (sink.m_recorded.size() != 2u) {
        throw std::runtime_error("expected two kv range Delete ops");
    }
    for (const sync::ChangeOp& op : sink.m_recorded) {
        if (op.dbi_name != "range_kv" ||
            op.op_type != sync::ChangeOpType::Delete ||
            op.storage_key.empty()) {
            throw std::runtime_error("kv range Delete capture incorrect");
        }
    }

    conn->detach_sync_capture();
    conn->disconnect();
    cleanup(p);
}

void test_vector_store_writes_via_sink() {
    using namespace mdbxc;
    const std::string p = "test_capture_vector_store.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 16;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    conn->attach_sync_capture(&sink);

    {
        VectorStore store(conn, "sync_vector");
        const uint64_t id = store.add(make_embedding({ 1.0f, 0.0f }),
                                      "document", "{}");
        if (id != 0u) {
            throw std::runtime_error("first vector id should be zero");
        }
    }

    conn->detach_sync_capture();
    conn->disconnect();
    cleanup(p);

    if (sink.m_recorded.size() != 4u) {
        throw std::runtime_error("expected four vector store ops, got " +
                                 std::to_string(sink.m_recorded.size()));
    }
    const char* expected_dbi[] = {
        "vectors_sync_vector_ids",
        "vectors_sync_vector_embeddings",
        "vectors_sync_vector_texts",
        "vectors_sync_vector_metadata"
    };
    for (std::size_t i = 0; i < sink.m_recorded.size(); ++i) {
        const sync::ChangeOp& op = sink.m_recorded[i];
        if (op.dbi_name != expected_dbi[i]) {
            throw std::runtime_error("vector store dbi order/name incorrect");
        }
        if (op.op_type != sync::ChangeOpType::Put ||
            op.storage_key.empty() ||
            op.value.empty()) {
            throw std::runtime_error("vector store Put capture incorrect");
        }
        if ((op.dbi_flags & MDBX_INTEGERKEY) == 0) {
            throw std::runtime_error("vector store dbi_flags missing MDBX_INTEGERKEY");
        }
    }
}

void test_specialized_tables_do_not_capture_in_v01() {
    using namespace mdbxc;
    const std::string p = "test_capture_specialized_deferred.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 16;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    conn->attach_sync_capture(&sink);

    auto assert_no_capture = [&sink](const char* table_name) {
        if (!sink.m_recorded.empty()) {
            throw std::runtime_error(
                std::string(table_name) +
                " must not emit sync ChangeOp in v0.1 without wire-format support"
            );
        }
    };

    {
        AnyValueTable<int> any_values(conn, "any_values");
        any_values.set(1, std::string("one"));
        assert_no_capture("AnyValueTable");

        KeyMultiValueTable<int, std::string> multi_values(conn, "multi_values");
        multi_values.insert(1, "one");
        assert_no_capture("KeyMultiValueTable");
        multi_values.insert(1, "uno");
        assert_no_capture("KeyMultiValueTable");

        HashedKeyValueStore<std::string, std::string> hashed(conn, "hashed_values");
        hashed.insert_or_assign("one", "uno");
        assert_no_capture("HashedKeyValueStore");
    }

    conn->detach_sync_capture();
    conn->disconnect();
    cleanup(p);
}

void test_capture_flush_on_commit() {
    using namespace mdbxc;
    const std::string p = "test_capture_flush.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    StubSink sink;
    conn->attach_sync_capture(&sink);

    {
        KeyTable<int> t(conn, "t2");
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        t.insert(7);
        txn.commit();
    }

    conn->detach_sync_capture();
    cleanup(p);

    if (sink.m_flushed.empty()) {
        throw std::runtime_error("flush_in_txn was not called on Transaction::commit");
    }
}

void test_changelog_capture_roundtrip() {
    using namespace mdbxc;
    const std::string p = "test_capture_changelog.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    /// SyncEngine normally writes node_id before capture is enabled. We
    /// simulate that by writing the node id through MetaStore directly.
    const sync::NodeId node_id = [] {
        sync::NodeId n{};
        for (int i = 0; i < 16; ++i) {
            n[i] = static_cast<std::uint8_t>(0xC0 + i);
        }
        return n;
    }();

    {
        sync::MetaStore meta(conn->env_handle());
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        meta.open(txn.handle());
        meta.set_node_id(txn.handle(), node_id);
        txn.commit();
    }

    sync::ThreadLocalChangeAccumulator sink(conn);
    conn->attach_sync_capture(&sink);

    {
        KeyValueTable<int, int> kv(conn, "data");
        kv.insert_or_assign(1, 100);
        kv.insert_or_assign(2, 200);
    }

    conn->detach_sync_capture();

    sync::ChangeLogStore cl(conn->env_handle());
    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        cl.open(txn.handle());
        txn.commit();
    }
    {
        auto txn = conn->transaction(TransactionMode::READ_ONLY);
        if (!cl.contains(txn.handle(), node_id, 1)) {
            throw std::runtime_error("changelog missing seq 1");
        }
        if (!cl.contains(txn.handle(), node_id, 2)) {
            throw std::runtime_error("changelog missing seq 2");
        }
        if (cl.contains(txn.handle(), node_id, 3)) {
            throw std::runtime_error("changelog unexpectedly has seq 3");
        }
    }

    conn->disconnect();
    cleanup(p);
}

void test_zero_node_id_rejected() {
    using namespace mdbxc;
    const std::string p = "test_capture_zero_node_id.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    sync::ThreadLocalChangeAccumulator sink(conn);
    conn->attach_sync_capture(&sink);

    bool caught = false;
    try {
        KeyValueTable<int, int> kv(conn, "t");
        kv.insert_or_assign(1, 100);
    } catch (const std::runtime_error&) {
        caught = true;
    }

    conn->detach_sync_capture();
    conn->disconnect();
    cleanup(p);

    if (!caught) {
        throw std::runtime_error("capture must reject uninitialised node_id");
    }
}
void test_explicit_rollback_does_not_flush() {
    using namespace mdbxc;
    const std::string p = "test_capture_rollback.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    /// Pre-init node_id.
    {
        sync::MetaStore meta(conn->env_handle());
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        meta.open(txn.handle());
        sync::NodeId n{};
        n[0] = 0xA1;
        meta.set_node_id(txn.handle(), n);
        txn.commit();
    }

    sync::ThreadLocalChangeAccumulator sink(conn);
    conn->attach_sync_capture(&sink);

    KeyValueTable<int, int> kv(conn, "t");
    {
        kv.insert_or_assign(1, 100);
    }
    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        kv.insert_or_assign(2, 200);
        txn.rollback();
    }
    {
        kv.insert_or_assign(3, 300);
    }

    conn->detach_sync_capture();

    sync::ChangeLogStore cl(conn->env_handle());
    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        cl.open(txn.handle());
        txn.commit();
    }

    auto key_bytes = [](int k) {
        std::vector<std::uint8_t> out(sizeof(int));
        std::memcpy(out.data(), &k, sizeof(int));
        return out;
    };

    std::vector<std::uint8_t> batch1, batch2;
    {
        auto txn = conn->transaction(TransactionMode::READ_ONLY);
        sync::NodeId n{};
        n[0] = 0xA1;
        if (!cl.get(txn.handle(), n, 1, batch1)) {
            throw std::runtime_error("changelog missing seq 1");
        }
        if (!cl.get(txn.handle(), n, 2, batch2)) {
            throw std::runtime_error("changelog missing seq 2");
        }
    }

    auto batch_has_key = [](const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& key) {
        const sync::ChangeBatch b = sync::ChangeBatchCodec::decode_exact(bytes);
        for (const sync::ChangeOp& op : b.ops) {
            if (op.dbi_name == "t" && op.storage_key == key) return true;
        }
        return false;
    };

    if (!batch_has_key(batch1, key_bytes(1))) throw std::runtime_error("seq 1 should contain key=1");
    if (batch_has_key(batch1, key_bytes(2))) throw std::runtime_error("seq 1 should not contain rolled-back key=2");
    if (!batch_has_key(batch2, key_bytes(3))) throw std::runtime_error("seq 2 should contain key=3");
    if (batch_has_key(batch2, key_bytes(2))) throw std::runtime_error("seq 2 leaked rolled-back key=2");

    conn->disconnect();
    cleanup(p);
}


void test_aborted_transaction_does_not_flush() {
    using namespace mdbxc;
    const std::string p = "test_capture_aborted.mdbx";
    cleanup(p);

    Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = Connection::create(cfg);

    /// Pre-init node_id so capture does not throw on the first attempt.
    {
        sync::MetaStore meta(conn->env_handle());
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        meta.open(txn.handle());
        sync::NodeId n{};
        n[0] = 0xA1;
        meta.set_node_id(txn.handle(), n);
        txn.commit();
    }

    sync::ThreadLocalChangeAccumulator sink(conn);
    conn->attach_sync_capture(&sink);

    /// One KeyValueTable instance shared between the two writes; the DBI
    /// handle stays valid across both transactions because MDBX caches DBIs
    /// by name in the environment.
    KeyValueTable<int, int> kv(conn, "t");
    {
        kv.insert_or_assign(1, 100);
    }
    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        kv.insert_or_assign(2, 200);
        /// txn aborts on destruction without commit.
    }
    {
        kv.insert_or_assign(3, 300);
    }

    conn->detach_sync_capture();

    /// Decode every committed ChangeBatch and ensure the aborted op (key=2)
    /// does not appear. Also confirm that seq 1 carries op=1 and seq 2
    /// carries op=3 (the post-abort commit).
    sync::ChangeLogStore cl(conn->env_handle());
    {
        auto txn = conn->transaction(TransactionMode::WRITABLE);
        cl.open(txn.handle());
        txn.commit();
    }

    auto key_bytes = [](int k) {
        std::vector<std::uint8_t> out(sizeof(int));
        std::memcpy(out.data(), &k, sizeof(int));
        return out;
    };

    std::vector<std::uint8_t> batch1, batch2;
    {
        auto txn = conn->transaction(TransactionMode::READ_ONLY);
        sync::NodeId n{};
        n[0] = 0xA1;
        if (!cl.get(txn.handle(), n, 1, batch1)) {
            throw std::runtime_error("changelog missing seq 1 from first commit");
        }
        if (!cl.get(txn.handle(), n, 2, batch2)) {
            throw std::runtime_error("changelog missing seq 2 from post-abort commit");
        }
    }

    auto batch_has_key = [](const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& key) {
        const sync::ChangeBatch b = sync::ChangeBatchCodec::decode_exact(bytes);
        for (const sync::ChangeOp& op : b.ops) {
            if (op.dbi_name == "t" && op.storage_key == key) {
                return true;
            }
        }
        return false;
    };

    if (!batch_has_key(batch1, key_bytes(1))) {
        throw std::runtime_error("seq 1 should contain op with key=1");
    }
    if (batch_has_key(batch1, key_bytes(2))) {
        throw std::runtime_error("seq 1 should not contain aborted op with key=2");
    }
    if (!batch_has_key(batch2, key_bytes(3))) {
        throw std::runtime_error("seq 2 should contain op with key=3 (post-abort commit)");
    }
    if (batch_has_key(batch2, key_bytes(2))) {
        throw std::runtime_error("seq 2 leaked aborted op with key=2");
    }

    conn->disconnect();
    cleanup(p);
}

} // namespace

int main() {
    struct Case {
        const char* name;
        void (*fn)();
    };

    const Case cases[] = {
        { "test_no_sink_no_capture", &test_no_sink_no_capture },
        { "test_capture_writes_via_sink", &test_capture_writes_via_sink },
        { "test_capture_scope_restores_previous_sink",
          &test_capture_scope_restores_previous_sink },
        { "test_capture_scope_nested_scopes_restore_in_order",
          &test_capture_scope_nested_scopes_restore_in_order },
        { "test_capture_scope_rejects_out_of_order_detach",
          &test_capture_scope_rejects_out_of_order_detach },
        { "test_capture_scope_rejects_same_sink_out_of_order_detach",
          &test_capture_scope_rejects_same_sink_out_of_order_detach },
        { "test_capture_scope_detach_restores_null_once",
          &test_capture_scope_detach_restores_null_once },
        { "test_capture_scope_rejects_null_arguments",
          &test_capture_scope_rejects_null_arguments },
        { "test_sequence_set_writes_via_sink", &test_sequence_set_writes_via_sink },
        { "test_value_table_writes_storage_key_via_sink",
          &test_value_table_writes_storage_key_via_sink },
        { "test_key_value_bulk_writes_via_sink",
          &test_key_value_bulk_writes_via_sink },
        { "test_range_erase_writes_via_sink", &test_range_erase_writes_via_sink },
        { "test_vector_store_writes_via_sink",
          &test_vector_store_writes_via_sink },
        { "test_capture_flush_on_commit", &test_capture_flush_on_commit },
        { "test_specialized_tables_do_not_capture_in_v01",
          &test_specialized_tables_do_not_capture_in_v01 },
        { "test_changelog_capture_roundtrip", &test_changelog_capture_roundtrip },
        { "test_zero_node_id_rejected", &test_zero_node_id_rejected },
        { "test_aborted_transaction_does_not_flush",
          &test_aborted_transaction_does_not_flush },
        { "test_explicit_rollback_does_not_flush",
          &test_explicit_rollback_does_not_flush }
    };

    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        try {
            cases[i].fn();
            std::printf("PASS %s\n", cases[i].name);
        } catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", cases[i].name, e.what());
            return static_cast<int>(i + 1u);
        } catch (...) {
            std::printf("FAIL %s: non-std exception\n", cases[i].name);
            return static_cast<int>(i + 1u);
        }
    }

    return 0;
}
