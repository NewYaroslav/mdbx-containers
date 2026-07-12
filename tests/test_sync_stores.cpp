#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>
#include <cstdio>
#include <cstdint>
#include <cstring>
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

std::vector<std::uint8_t> make_changelog_key(const mdbxc::sync::NodeId& origin,
                                             std::uint64_t seq) {
    std::vector<std::uint8_t> out(24);
    std::memcpy(out.data(), origin.data(), 16);
    for (int i = 0; i < 8; ++i) {
        out[16 + i] =
            static_cast<std::uint8_t>((seq >> ((7 - i) * 8)) & 0xff);
    }
    return out;
}

void put_raw_changelog(MDBX_txn* txn,
                       MDBX_dbi dbi,
                       const mdbxc::sync::NodeId& origin,
                       std::uint64_t seq,
                       const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint8_t> key = make_changelog_key(origin, seq);
    MDBX_val k = { key.empty() ? nullptr : &key[0], key.size() };
    MDBX_val v = { bytes.empty() ? nullptr : const_cast<std::uint8_t*>(&bytes[0]),
                   bytes.size() };
    mdbxc::check_mdbx(mdbx_put(txn, dbi, &k, &v, MDBX_NOOVERWRITE),
                      "raw changelog put failed");
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

void test_origin_index_store() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_origins.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    OriginIndexStore store(conn->env_handle());
    const NodeId origin_a = make_node(0x10);
    const NodeId origin_b = make_node(0x20);

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        store.open(txn.handle());
        if (!store.empty(txn.handle())) {
            throw std::runtime_error("new origin index should be empty");
        }
        store.note_origin(txn.handle(), origin_a, 7);
        store.note_origin(txn.handle(), origin_b, 3);
        store.note_origin(txn.handle(), origin_a, 2);
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        OriginIndexStore ro(conn->env_handle());
        if (!ro.open_existing(txn.handle())) {
            throw std::runtime_error("origin index should exist");
        }
        std::uint64_t seq = 0;
        if (!ro.last_seq(txn.handle(), origin_a, seq) || seq != 7u) {
            throw std::runtime_error("origin_a last seq mismatch");
        }
        if (!ro.last_seq(txn.handle(), origin_b, seq) || seq != 3u) {
            throw std::runtime_error("origin_b last seq mismatch");
        }
        const std::vector<NodeId> origins = ro.origins(txn.handle());
        if (origins.size() != 2u ||
            origins[0] != origin_a ||
            origins[1] != origin_b) {
            throw std::runtime_error("origin index order mismatch");
        }
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        OriginIndexStore writable(conn->env_handle());
        writable.open(txn.handle());
        const std::size_t removed = writable.clear(txn.handle());
        if (removed != 2u || !writable.empty(txn.handle())) {
            throw std::runtime_error("origin index clear mismatch");
        }
        txn.commit();
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

void test_changelog_updates_origin_index() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_changelog_origins.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    ChangeLogStore log(conn->env_handle());
    const NodeId origin = make_node(0x30);

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        log.open(txn.handle());
        log.append(txn.handle(), origin, 1, { 0x01 });
        log.append(txn.handle(), origin, 3, { 0x03 });
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        OriginIndexStore origins(conn->env_handle());
        if (!origins.open_existing(txn.handle())) {
            throw std::runtime_error("origin index should exist after append");
        }
        std::uint64_t seq = 0;
        if (!origins.last_seq(txn.handle(), origin, seq) || seq != 3u) {
            throw std::runtime_error("changelog did not update origin tail");
        }
    }

    conn->disconnect();
    cleanup(p);
}

void test_changelog_backfills_legacy_origins_on_append() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_changelog_origin_backfill.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    const NodeId origin_a = make_node(0x10);
    const NodeId origin_b = make_node(0x40);

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        MDBX_dbi raw = 0;
        mdbxc::check_mdbx(mdbx_dbi_open(txn.handle(), "_mdbxc_changelog",
                                        MDBX_CREATE, &raw),
                          "open raw changelog failed");
        put_raw_changelog(txn.handle(), raw, origin_a, 1, { 0xA1 });
        put_raw_changelog(txn.handle(), raw, origin_a, 2, { 0xA2 });

        ChangeLogStore log(conn->env_handle());
        log.open(txn.handle());
        log.append(txn.handle(), origin_b, 1, { 0xB1 });
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        OriginIndexStore origins(conn->env_handle());
        if (!origins.open_existing(txn.handle())) {
            throw std::runtime_error("origin index should exist after backfill");
        }
        std::uint64_t seq = 0;
        if (!origins.last_seq(txn.handle(), origin_a, seq) || seq != 2u) {
            throw std::runtime_error("legacy origin was not backfilled");
        }
        if (!origins.last_seq(txn.handle(), origin_b, seq) || seq != 1u) {
            throw std::runtime_error("new origin was not indexed");
        }
    }

    conn->disconnect();
    cleanup(p);
}

void test_changelog_rebuilds_partial_origin_index() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_changelog_origin_rebuild.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    const NodeId origin_a = make_node(0x10);
    const NodeId origin_b = make_node(0x40);
    const NodeId stale_origin = make_node(0x70);

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        MDBX_dbi raw = 0;
        mdbxc::check_mdbx(mdbx_dbi_open(txn.handle(), "_mdbxc_changelog",
                                        MDBX_CREATE, &raw),
                          "open raw changelog failed");
        put_raw_changelog(txn.handle(), raw, origin_a, 1, { 0xA1 });
        put_raw_changelog(txn.handle(), raw, origin_a, 2, { 0xA2 });
        put_raw_changelog(txn.handle(), raw, origin_b, 1, { 0xB1 });

        OriginIndexStore origins(conn->env_handle());
        origins.open(txn.handle());
        origins.note_origin(txn.handle(), origin_a, 2);
        origins.note_origin(txn.handle(), stale_origin, 9);
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        ChangeLogStore log(conn->env_handle());
        log.open(txn.handle());
        if (log.origin_index_matches_changelog(txn.handle())) {
            throw std::runtime_error("partial origin index should not validate");
        }
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        ChangeLogStore log(conn->env_handle());
        log.open(txn.handle());
        const std::size_t origins_written = log.rebuild_origin_index(txn.handle());
        if (origins_written != 2u) {
            throw std::runtime_error("origin rebuild wrote wrong count");
        }
        if (!log.origin_index_matches_changelog(txn.handle())) {
            throw std::runtime_error("rebuilt origin index should validate");
        }
        txn.commit();
    }

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        OriginIndexStore origins(conn->env_handle());
        if (!origins.open_existing(txn.handle())) {
            throw std::runtime_error("rebuilt origin index should exist");
        }
        std::uint64_t seq = 0;
        if (!origins.last_seq(txn.handle(), origin_a, seq) || seq != 2u) {
            throw std::runtime_error("rebuilt origin_a tail mismatch");
        }
        if (!origins.last_seq(txn.handle(), origin_b, seq) || seq != 1u) {
            throw std::runtime_error("rebuilt origin_b tail mismatch");
        }
        if (origins.last_seq(txn.handle(), stale_origin, seq)) {
            throw std::runtime_error("rebuilt index kept stale origin");
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

void test_changelog_prune_up_to_boundary() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_prune.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    ChangeLogStore store(conn->env_handle());
    const NodeId origin = make_node(0xA1);

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        store.open(txn.handle());
        for (std::uint64_t s : { 1ULL, 2ULL, 255ULL, 256ULL, 257ULL, 1000ULL }) {
            store.append(txn.handle(), origin, s, { 0x01, static_cast<std::uint8_t>(s & 0xff) });
        }

        const std::size_t removed = store.prune_up_to(txn.handle(), origin, 256);
        if (removed != 4) {
            throw std::runtime_error("expected 4 removed, got " + std::to_string(removed));
        }

        for (std::uint64_t s : { 1ULL, 2ULL, 255ULL, 256ULL }) {
            if (store.contains(txn.handle(), origin, s)) {
                throw std::runtime_error("seq " + std::to_string(s) + " should be pruned");
            }
        }
        for (std::uint64_t s : { 257ULL, 1000ULL }) {
            if (!store.contains(txn.handle(), origin, s)) {
                throw std::runtime_error("seq " + std::to_string(s) + " should remain");
            }
        }
        txn.commit();
    }

    conn->disconnect();
    cleanup(p);
}

void test_changelog_prune_does_not_touch_other_origin() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_prune_other.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    ChangeLogStore store(conn->env_handle());
    const NodeId origin_a = make_node(0xA2);
    const NodeId origin_b = make_node(0xB2);

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        store.open(txn.handle());
        store.append(txn.handle(), origin_a, 1, { 0x01 });
        store.append(txn.handle(), origin_b, 1, { 0x02 });
        store.append(txn.handle(), origin_a, 2, { 0x03 });

        const std::size_t removed_a = store.prune_up_to(txn.handle(), origin_a, 1);
        if (removed_a != 1) {
            throw std::runtime_error("expected 1 from origin_a, got " + std::to_string(removed_a));
        }
        if (!store.contains(txn.handle(), origin_b, 1)) {
            throw std::runtime_error("origin_b seq 1 was pruned unexpectedly");
        }
        if (!store.contains(txn.handle(), origin_a, 2)) {
            throw std::runtime_error("origin_a seq 2 must remain");
        }
        txn.commit();
    }

    conn->disconnect();
    cleanup(p);
}

void test_identity_key_collision() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_collision.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    IdentityIndexStore store(conn->env_handle());

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        store.open(txn.handle());

        IdentityIndexValue v;
        v.storage_key = { 0xAA };
        v.origin_node_id = make_node(0xC3);
        v.seq = 1;
        store.put(txn.handle(), "ab",  { 'c' }, v);
        store.put(txn.handle(), "a",   { 'b', 'c' }, v);

        IdentityIndexValue got1, got2;
        if (!store.get(txn.handle(), "ab", { 'c' }, got1)) {
            throw std::runtime_error("ab/c not found");
        }
        if (!store.get(txn.handle(), "a", { 'b', 'c' }, got2)) {
            throw std::runtime_error("a/bc not found");
        }

        IdentityIndexValue canon;
        canon.storage_key = { 0xAA };
        canon.origin_node_id = make_node(0xC3);
        canon.seq = 1;
        if (got1.storage_key != canon.storage_key ||
            got2.storage_key != canon.storage_key) {
            throw std::runtime_error("collision: two distinct keys collapsed to one record");
        }
        txn.commit();
    }

    conn->disconnect();
    cleanup(p);
}

template<class Store, class Op>
void expect_open_required(const std::string& label, Store& store, Op op) {
    bool caught = false;
    try {
        op();
    } catch (const std::logic_error&) {
        caught = true;
    } catch (const mdbxc::MdbxException&) {
        caught = true;
    }
    if (!caught) {
        throw std::runtime_error(label + ": expected logic_error or MdbxException before open()");
    }
}

void test_stores_require_open() {
    using namespace mdbxc::sync;
    const std::string p = "test_sync_stores_open_required.mdbx";
    cleanup(p);

    mdbxc::Config cfg;
    cfg.pathname = p;
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    auto conn = mdbxc::Connection::create(cfg);

    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);

        MetaStore meta(conn->env_handle());
        expect_open_required("MetaStore::get_db_uuid", meta,
                            [&] { (void)meta.get_db_uuid(txn.handle()); });
        expect_open_required("MetaStore::set_db_uuid", meta,
                            [&] { meta.set_db_uuid(txn.handle(), make_node(0xA0)); });
        expect_open_required("MetaStore::get_local_seq", meta,
                            [&] { (void)meta.get_local_seq(txn.handle()); });
        expect_open_required("MetaStore::increment_local_seq", meta,
                            [&] { (void)meta.increment_local_seq(txn.handle()); });

        AppliedStore applied(conn->env_handle());
        expect_open_required("AppliedStore::last_applied_seq", applied,
                            [&] { (void)applied.last_applied_seq(txn.handle(), make_node(0xB0)); });
        expect_open_required("AppliedStore::set_last_applied_seq", applied,
                            [&] { applied.set_last_applied_seq(txn.handle(), make_node(0xB0), 1); });

        ChangeLogStore change(conn->env_handle());
        expect_open_required("ChangeLogStore::append", change,
                            [&] { change.append(txn.handle(), make_node(0xC0), 1, { 0x01 }); });
        expect_open_required("ChangeLogStore::contains", change,
                            [&] { (void)change.contains(txn.handle(), make_node(0xC0), 1); });
        expect_open_required("ChangeLogStore::erase", change,
                            [&] { (void)change.erase(txn.handle(), make_node(0xC0), 1); });
        expect_open_required("ChangeLogStore::prune_up_to", change,
                            [&] { (void)change.prune_up_to(txn.handle(), make_node(0xC0), 1); });
        expect_open_required("ChangeLogStore::origin_index_matches_changelog", change,
                            [&change, &txn] { (void)change.origin_index_matches_changelog(txn.handle()); });
        expect_open_required("ChangeLogStore::rebuild_origin_index", change,
                            [&change, &txn] { (void)change.rebuild_origin_index(txn.handle()); });

        OriginIndexStore origins(conn->env_handle());
        std::uint64_t seq = 0;
        expect_open_required("OriginIndexStore::empty", origins,
                            [&origins, &txn] { (void)origins.empty(txn.handle()); });
        expect_open_required("OriginIndexStore::clear", origins,
                            [&origins, &txn] { (void)origins.clear(txn.handle()); });
        expect_open_required("OriginIndexStore::note_origin", origins,
                            [&origins, &txn] { origins.note_origin(txn.handle(), make_node(0xD0), 1); });
        expect_open_required("OriginIndexStore::last_seq", origins,
                            [&origins, &txn, &seq] { (void)origins.last_seq(txn.handle(), make_node(0xD0), seq); });
        expect_open_required("OriginIndexStore::origins", origins,
                            [&origins, &txn] { (void)origins.origins(txn.handle()); });

        IdentityIndexStore identity(conn->env_handle());
        IdentityIndexValue iv;
        expect_open_required("IdentityIndexStore::put", identity,
                            [&] { identity.put(txn.handle(), "t", { 0x01 }, iv); });
        expect_open_required("IdentityIndexStore::get", identity,
                            [&] { (void)identity.get(txn.handle(), "t", { 0x01 }, iv); });
        expect_open_required("IdentityIndexStore::tombstone", identity,
                            [&] { identity.tombstone(txn.handle(), "t", { 0x01 }, iv); });
        expect_open_required("IdentityIndexStore::erase", identity,
                            [&] { (void)identity.erase(txn.handle(), "t", { 0x01 }); });
    }

    conn->disconnect();
    cleanup(p);
}

} // namespace

int main() {
    test_meta_store();
    test_origin_index_store();
    test_changelog_store();
    test_changelog_updates_origin_index();
    test_changelog_backfills_legacy_origins_on_append();
    test_changelog_rebuilds_partial_origin_index();
    test_applied_store();
    test_identity_index_store();
    test_changelog_prune_up_to_boundary();
    test_changelog_prune_does_not_touch_other_origin();
    test_identity_key_collision();
    test_stores_require_open();
    return 0;
}
