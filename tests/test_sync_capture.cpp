#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>
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

    conn->detach_sync_capture();
    conn->disconnect();
    cleanup(p);

    if (sink.m_recorded.size() != 1u) {
        throw std::runtime_error("expected one sequence set op, got " +
                                 std::to_string(sink.m_recorded.size()));
    }
    const sync::ChangeOp& op = sink.m_recorded[0];
    if (op.op_type != sync::ChangeOpType::Put) {
        throw std::runtime_error("sequence set must capture Put");
    }
    if (op.dbi_name != "events") {
        throw std::runtime_error("sequence set dbi_name not propagated");
    }
    if (op.storage_key.empty() || op.value.empty()) {
        throw std::runtime_error("sequence set key/value not captured");
    }
    if ((op.dbi_flags & MDBX_INTEGERKEY) == 0) {
        throw std::runtime_error("sequence set dbi_flags missing MDBX_INTEGERKEY");
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
    try {
        test_no_sink_no_capture();
        std::printf("PASS test_no_sink_no_capture\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_no_sink_no_capture: %s\n", e.what());
        return 1;
    } catch (...) {
        std::printf("FAIL test_no_sink_no_capture: non-std exception\n");
        return 1;
    }
    try {
        test_capture_writes_via_sink();
        std::printf("PASS test_capture_writes_via_sink\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_capture_writes_via_sink: %s\n", e.what());
        return 2;
    } catch (...) {
        std::printf("FAIL test_capture_writes_via_sink: non-std exception\n");
        return 2;
    }
    try {
        test_sequence_set_writes_via_sink();
        std::printf("PASS test_sequence_set_writes_via_sink\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_sequence_set_writes_via_sink: %s\n", e.what());
        return 3;
    } catch (...) {
        std::printf("FAIL test_sequence_set_writes_via_sink: non-std exception\n");
        return 3;
    }
    try {
        test_value_table_writes_storage_key_via_sink();
        std::printf("PASS test_value_table_writes_storage_key_via_sink\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_value_table_writes_storage_key_via_sink: %s\n", e.what());
        return 4;
    } catch (...) {
        std::printf("FAIL test_value_table_writes_storage_key_via_sink: non-std exception\n");
        return 4;
    }
    try {
        test_key_value_bulk_writes_via_sink();
        std::printf("PASS test_key_value_bulk_writes_via_sink\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_key_value_bulk_writes_via_sink: %s\n", e.what());
        return 5;
    } catch (...) {
        std::printf("FAIL test_key_value_bulk_writes_via_sink: non-std exception\n");
        return 5;
    }
    try {
        test_range_erase_writes_via_sink();
        std::printf("PASS test_range_erase_writes_via_sink\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_range_erase_writes_via_sink: %s\n", e.what());
        return 6;
    } catch (...) {
        std::printf("FAIL test_range_erase_writes_via_sink: non-std exception\n");
        return 6;
    }
    try {
        test_vector_store_writes_via_sink();
        std::printf("PASS test_vector_store_writes_via_sink\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_vector_store_writes_via_sink: %s\n", e.what());
        return 7;
    } catch (...) {
        std::printf("FAIL test_vector_store_writes_via_sink: non-std exception\n");
        return 7;
    }
    try {
        test_capture_flush_on_commit();
        std::printf("PASS test_capture_flush_on_commit\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_capture_flush_on_commit: %s\n", e.what());
        return 8;
    } catch (...) {
        std::printf("FAIL test_capture_flush_on_commit: non-std exception\n");
        return 8;
    }
    try {
        test_changelog_capture_roundtrip();
        std::printf("PASS test_changelog_capture_roundtrip\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_changelog_capture_roundtrip: %s\n", e.what());
        return 9;
    } catch (...) {
        std::printf("FAIL test_changelog_capture_roundtrip: non-std exception\n");
        return 9;
    }
    try {
        test_zero_node_id_rejected();
        std::printf("PASS test_zero_node_id_rejected\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_zero_node_id_rejected: %s\n", e.what());
        return 10;
    } catch (...) {
        std::printf("FAIL test_zero_node_id_rejected: non-std exception\n");
        return 10;
    }
    try {
        test_aborted_transaction_does_not_flush();
        std::printf("PASS test_aborted_transaction_does_not_flush\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_aborted_transaction_does_not_flush: %s\n", e.what());
        return 11;
    } catch (...) {
        std::printf("FAIL test_aborted_transaction_does_not_flush: non-std exception\n");
        return 11;
    }
    try {
        test_explicit_rollback_does_not_flush();
        std::printf("PASS test_explicit_rollback_does_not_flush\n");
    } catch (const std::exception& e) {
        std::printf("FAIL test_explicit_rollback_does_not_flush: %s\n", e.what());
        return 12;
    } catch (...) {
        std::printf("FAIL test_explicit_rollback_does_not_flush: non-std exception\n");
        return 12;
    }

    return 0;
}
