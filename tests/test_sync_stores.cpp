#include <mdbx_containers.hpp>
#include <mdbx_containers/Sync.hpp>
#include <cstdio>
#include <cstdint>
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

void test_meta_store() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_meta.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    MetaStore store(conn->env_handle());
    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        store.open(txn.handle());
        if (!store.is_open()) throw std::runtime_error("MetaStore not open");

        const NodeId db_uuid = make_node(0xA0);
        const NodeId node_id = make_node(0xB0);
        store.set_db_uuid(txn.handle(), db_uuid);
        store.set_node_id(txn.handle(), node_id);
        store.set_schema_version(txn.handle(), meta_schema_version());
        store.set_created_at_ms(txn.handle(), 1700000000000ULL);
        const std::uint64_t next = store.increment_local_seq(txn.handle());
        if (next != 1) throw std::runtime_error("local_seq start should be 1");
        store.increment_local_seq(txn.handle());
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        const NodeId db_uuid = store.get_db_uuid(txn.handle());
        const NodeId node_id = store.get_node_id(txn.handle());
        if (db_uuid != make_node(0xA0)) throw std::runtime_error("db_uuid mismatch");
        if (node_id != make_node(0xB0)) throw std::runtime_error("node_id mismatch");
        if (store.get_schema_version(txn.handle()) != meta_schema_version()) {
            throw std::runtime_error("schema_version mismatch");
        }
        if (store.get_local_seq(txn.handle()) != 2) {
            throw std::runtime_error("local_seq should be 2 after two increments");
        }
        if (store.get_created_at_ms(txn.handle()) != 1700000000000ULL) {
            throw std::runtime_error("created_at_ms mismatch");
        }
    }

    conn->disconnect();
    cleanup(p);
}

void test_changelog_store() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_changelog.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    ChangeLogStore store(conn->env_handle());
    const NodeId origin = make_node(0x10);

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        store.open(txn.handle());

        ChangeBatch batch;
        batch.origin_node_id = origin;
        batch.seq = 7;
        const std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch);
        store.append(txn.handle(), origin, 7, bytes);

        if (!store.contains(txn.handle(), origin, 7)) {
            throw std::runtime_error("contains should be true after append");
        }
        if (store.contains(txn.handle(), origin, 8)) {
            throw std::runtime_error("contains should be false for missing seq");
        }
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        std::vector<std::uint8_t> out;
        if (!store.get(txn.handle(), origin, 7, out)) {
            throw std::runtime_error("get should return true");
        }
        const ChangeBatch decoded = ChangeBatchCodec::decode_exact(out);
        if (decoded.seq != 7) throw std::runtime_error("decoded seq mismatch");
        if (decoded.origin_node_id != origin) {
            throw std::runtime_error("decoded origin mismatch");
        }
    }

    conn->disconnect();
    cleanup(p);
}

void test_applied_store() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_applied.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    AppliedStore store(conn->env_handle());
    const NodeId origin_a = make_node(0xC0);
    const NodeId origin_b = make_node(0xD0);

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        store.open(txn.handle());
        store.set_last_applied_seq(txn.handle(), origin_a, 42);
        store.set_last_applied_seq(txn.handle(), origin_b, 100);
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        if (store.last_applied_seq(txn.handle(), origin_a) != 42) {
            throw std::runtime_error("origin_a seq mismatch");
        }
        if (store.last_applied_seq(txn.handle(), origin_b) != 100) {
            throw std::runtime_error("origin_b seq mismatch");
        }
        const NodeId unknown{};
        if (store.last_applied_seq(txn.handle(), unknown) != 0) {
            throw std::runtime_error("unknown origin must be 0");
        }
    }

    conn->disconnect();
    cleanup(p);
}

void test_identity_index_store() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_identity.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    IdentityIndexStore store(conn->env_handle());
    const std::string dbi = "trades";
    const std::vector<std::uint8_t> id_key = { 'E','U','R','U','S','D' };
    const std::vector<std::uint8_t> sk = { 0x01, 0x02, 0x03 };

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        store.open(txn.handle());
        IdentityIndexValue v;
        v.storage_key = sk;
        v.origin_node_id = make_node(0xE0);
        v.seq = 9;
        v.revision_key = { 0xAA };
        store.put(txn.handle(), dbi, id_key, v);
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        IdentityIndexValue out;
        if (!store.get(txn.handle(), dbi, id_key, out)) {
            throw std::runtime_error("identity get should succeed");
        }
        if (out.storage_key != sk) throw std::runtime_error("storage_key mismatch");
        if (out.seq != 9) throw std::runtime_error("seq mismatch");
        if (out.revision_key.size() != 1 || out.revision_key[0] != 0xAA) {
            throw std::runtime_error("revision_key mismatch");
        }
        if (out.flags != 0) throw std::runtime_error("flags should be 0");
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        IdentityIndexValue marker;
        marker.origin_node_id = make_node(0xE0);
        marker.seq = 10;
        store.tombstone(txn.handle(), dbi, id_key, marker);
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        IdentityIndexValue out;
        if (!store.get(txn.handle(), dbi, id_key, out)) {
            throw std::runtime_error("tombstoned record should still be readable");
        }
        if ((out.flags & IDENTITY_TOMBSTONE) == 0) {
            throw std::runtime_error("tombstone flag must be set");
        }
        if (out.seq != 10) throw std::runtime_error("tombstone seq mismatch");
    }

    conn->disconnect();
    cleanup(p);
}

} // namespace

int main() {
    test_meta_store();
    test_changelog_store();
    test_applied_store();
    test_identity_index_store();
    return 0;
}