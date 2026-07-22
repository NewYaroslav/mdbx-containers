#include "test_assert.hpp"

#include <mdbx_containers/AnyValueTable.hpp>
#include <mdbx_containers/HashedKeyValueStore.hpp>
#include <mdbx_containers/KeyMultiValueTable.hpp>
#include <mdbx_containers/KeyOrderedMultiValueTable.hpp>
#include <mdbx_containers/KeyTable.hpp>
#include <mdbx_containers/KeyValueTable.hpp>
#include <mdbx_containers/SequenceTable.hpp>
#include <mdbx_containers/ValueTable.hpp>
#include <mdbx_containers/sync.hpp>

#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-lck").c_str());
}

mdbxc::Config config_for(const std::string& path) {
    mdbxc::Config cfg;
    cfg.pathname = path;
    cfg.max_dbs = 64;
    cfg.no_subdir = true;
    return cfg;
}

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId node{};
    for (std::size_t i = 0; i < node.size(); ++i) {
        node[i] = static_cast<std::uint8_t>(seed + i);
    }
    return node;
}

template<class Fn>
void expect_invalid_argument(const char* label, Fn fn) {
    bool thrown = false;
    try {
        fn();
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    if (!thrown) {
        throw std::runtime_error(
            std::string(label) + ": expected std::invalid_argument");
    }
}

template<class Fn>
void expect_invalid_argument_containing(const char* label,
                                        const char* needle,
                                        Fn fn) {
    try {
        fn();
    } catch (const std::invalid_argument& e) {
        const std::string message = e.what();
        if (message.find(needle) == std::string::npos) {
            throw std::runtime_error(
                std::string(label) + ": unexpected invalid_argument: " +
                message);
        }
        return;
    }
    throw std::runtime_error(
        std::string(label) + ": expected std::invalid_argument");
}

void test_public_tables_reject_foreign_transactions() {
    const std::string path_a = "test_transaction_provenance_tables_a.mdbx";
    const std::string path_b = "test_transaction_provenance_tables_b.mdbx";
    cleanup(path_a);
    cleanup(path_b);

    std::shared_ptr<mdbxc::Connection> conn_a =
        mdbxc::Connection::create(config_for(path_a));
    std::shared_ptr<mdbxc::Connection> conn_b =
        mdbxc::Connection::create(config_for(path_b));

    mdbxc::KeyValueTable<int, int> kv(conn_a, "kv");
    mdbxc::KeyTable<int> keys(conn_a, "keys");
    mdbxc::ValueTable<int> value(conn_a, "value");
    mdbxc::SequenceTable<int> sequence(conn_a, "sequence");
    mdbxc::AnyValueTable<int> any(conn_a, "any");
    mdbxc::KeyMultiValueTable<int, std::string> multi(conn_a, "multi");
    mdbxc::KeyOrderedMultiValueTable<int, std::string> ordered_multi(conn_a, "ordered_multi");
    mdbxc::HashedKeyValueStore<std::string, std::string> hashed(conn_a, "hashed");

    {
        mdbxc::Transaction txn = conn_a->transaction(mdbxc::TransactionMode::WRITABLE);
        kv.insert_or_assign(100, 200, txn.handle());
        value.set(5, txn.handle());
        txn.commit();
    }
    MDBXC_TEST_ASSERT(kv.contains(100));
    MDBXC_TEST_ASSERT(value.get() == 5);

    {
        mdbxc::Transaction foreign =
            conn_b->transaction(mdbxc::TransactionMode::WRITABLE);
        MDBX_txn* raw = foreign.handle();

        expect_invalid_argument("KeyValueTable raw txn",
                                [&kv, raw] { kv.insert_or_assign(1, 10, raw); });
        expect_invalid_argument("KeyTable raw txn",
                                [&keys, raw] { keys.insert(1, raw); });
        expect_invalid_argument("ValueTable raw txn",
                                [&value, raw] { value.set(10, raw); });
        expect_invalid_argument("SequenceTable raw txn",
                                [&sequence, raw] {
                                    sequence.insert_or_assign(1, 10, raw);
                                });
        expect_invalid_argument("AnyValueTable raw txn",
                                [&any, raw] {
                                    any.set<std::string>(1, std::string("value"), raw);
                                });
        expect_invalid_argument("KeyMultiValueTable raw txn",
                                [&multi, raw] {
                                    multi.insert(1, std::string("value"), raw);
                                });
        expect_invalid_argument("KeyOrderedMultiValueTable raw txn",
                                [&ordered_multi, raw] {
                                    ordered_multi.append(1, std::string("value"), raw);
                                });
        expect_invalid_argument("HashedKeyValueStore raw txn",
                                [&hashed, raw] {
                                    hashed.insert_or_assign(std::string("key"),
                                                            std::string("value"),
                                                            raw);
                                });
        foreign.commit();
    }

    MDBXC_TEST_ASSERT(kv.count() == 1u);
    MDBXC_TEST_ASSERT(keys.empty());
    MDBXC_TEST_ASSERT(value.get() == 5);
    MDBXC_TEST_ASSERT(sequence.count() == 0u);
    MDBXC_TEST_ASSERT(!any.contains(1));
    MDBXC_TEST_ASSERT(multi.count() == 0u);
    MDBXC_TEST_ASSERT(ordered_multi.count() == 0u);
    MDBXC_TEST_ASSERT(hashed.count() == 0u);

    conn_a->disconnect();
    conn_b->disconnect();
    cleanup(path_a);
    cleanup(path_b);
}

void open_sync_stores(mdbxc::Connection& conn,
                      mdbxc::sync::MetaStore& meta,
                      mdbxc::sync::AppliedStore& applied,
                      mdbxc::sync::ChangeLogStore& change_log,
                      mdbxc::sync::OriginIndexStore& origins,
                      mdbxc::sync::IdentityIndexStore& identities) {
    mdbxc::Transaction txn = conn.transaction(mdbxc::TransactionMode::WRITABLE);
    meta.open(txn.handle());
    applied.open(txn.handle());
    change_log.open(txn.handle());
    origins.open(txn.handle());
    identities.open(txn.handle());
    txn.commit();
}

void test_sync_components_reject_foreign_transactions() {
    using namespace mdbxc::sync;

    const std::string path_a = "test_transaction_provenance_sync_a.mdbx";
    const std::string path_b = "test_transaction_provenance_sync_b.mdbx";
    cleanup(path_a);
    cleanup(path_b);

    std::shared_ptr<mdbxc::Connection> conn_a =
        mdbxc::Connection::create(config_for(path_a));
    std::shared_ptr<mdbxc::Connection> conn_b =
        mdbxc::Connection::create(config_for(path_b));

    const NodeId local_node = make_node(0x10);
    const NodeId remote_node = make_node(0x20);
    const NodeId db_id = make_node(0xD0);

    SyncEngine engine(conn_a);
    engine.initialize_local_identity(local_node, db_id);

    ChangeBatch batch;
    batch.version = 1;
    batch.origin_node_id = remote_node;
    batch.seq = 1;

    MetaStore meta(conn_a->env_handle());
    AppliedStore applied(conn_a->env_handle());
    ChangeLogStore change_log(conn_a->env_handle());
    OriginIndexStore origins(conn_a->env_handle());
    IdentityIndexStore identities(conn_a->env_handle());
    open_sync_stores(*conn_a, meta, applied, change_log, origins, identities);

    ThreadLocalChangeAccumulator accumulator(conn_a);

    {
        mdbxc::Transaction foreign =
            conn_b->transaction(mdbxc::TransactionMode::WRITABLE);
        MDBX_txn* raw = foreign.handle();

        expect_invalid_argument("SyncEngine::apply_batch_ex",
                                [&engine, raw, &batch] {
                                    engine.apply_batch_ex(raw, batch);
                                });
        expect_invalid_argument("SyncEngine::apply_batch",
                                [&engine, raw, &batch] {
                                    engine.apply_batch(raw, batch);
                                });
        expect_invalid_argument("SyncEngine::applied_cursor",
                                [&engine, raw] { engine.applied_cursor(raw); });
        expect_invalid_argument_containing(
            "SyncEngine::pull_changelog_page",
            "SyncEngine::pull_changelog_page",
            [&engine, raw] {
                PullRequest request;
                request.max_batches = 10;
                request.max_bytes = 1024;
                engine.pull_changelog_page(raw, 0, request);
            });

        expect_invalid_argument("MetaStore::get_schema_version",
                                [&meta, raw] { meta.get_schema_version(raw); });
        expect_invalid_argument("MetaStore::set_schema_version",
                                [&meta, raw] { meta.set_schema_version(raw, 1); });
        expect_invalid_argument("AppliedStore::last_applied_seq",
                                [&applied, raw, &remote_node] {
                                    applied.last_applied_seq(raw, remote_node);
                                });
        expect_invalid_argument("AppliedStore::set_last_applied_seq",
                                [&applied, raw, &remote_node] {
                                    applied.set_last_applied_seq(raw, remote_node, 1);
                                });
        expect_invalid_argument("AppliedStore::clear",
                                [&applied, raw, &remote_node] {
                                    applied.clear(raw, remote_node);
                                });

        const std::vector<std::uint8_t> bytes(1u, 0x42u);
        expect_invalid_argument("ChangeLogStore::append",
                                [&change_log, raw, &remote_node, &bytes] {
                                    change_log.append(raw, remote_node, 1, bytes);
                                });
        expect_invalid_argument("ChangeLogStore::contains",
                                [&change_log, raw, &remote_node] {
                                    change_log.contains(raw, remote_node, 1);
                                });
        expect_invalid_argument("ChangeLogStore::get",
                                [&change_log, raw, &remote_node] {
                                    std::vector<std::uint8_t> out;
                                    change_log.get(raw, remote_node, 1, out);
                                });
        expect_invalid_argument("ChangeLogStore::erase",
                                [&change_log, raw, &remote_node] {
                                    change_log.erase(raw, remote_node, 1);
                                });
        expect_invalid_argument("ChangeLogStore::prune_up_to",
                                [&change_log, raw, &remote_node] {
                                    change_log.prune_up_to(raw, remote_node, 1);
                                });
        expect_invalid_argument("ChangeLogStore::origin_index_matches_changelog",
                                [&change_log, raw] {
                                    change_log.origin_index_matches_changelog(raw);
                                });
        expect_invalid_argument("ChangeLogStore::rebuild_origin_index",
                                [&change_log, raw] {
                                    change_log.rebuild_origin_index(raw);
                                });

        std::uint64_t seq = 0;
        expect_invalid_argument("OriginIndexStore::empty",
                                [&origins, raw] { origins.empty(raw); });
        expect_invalid_argument("OriginIndexStore::clear",
                                [&origins, raw] { origins.clear(raw); });
        expect_invalid_argument("OriginIndexStore::note_origin",
                                [&origins, raw, &remote_node] {
                                    origins.note_origin(raw, remote_node, 1);
                                });
        expect_invalid_argument("OriginIndexStore::last_seq",
                                [&origins, raw, &remote_node, &seq] {
                                    origins.last_seq(raw, remote_node, seq);
                                });
        expect_invalid_argument("OriginIndexStore::origin_tails",
                                [&origins, raw] { origins.origin_tails(raw); });
        expect_invalid_argument("OriginIndexStore::origins",
                                [&origins, raw] { origins.origins(raw); });

        IdentityIndexValue identity_value;
        identity_value.origin_node_id = remote_node;
        identity_value.seq = 1;
        const std::vector<std::uint8_t> identity_key(1u, 0x01u);
        expect_invalid_argument("IdentityIndexStore::put",
                                [&identities, raw, &identity_key, &identity_value] {
                                    identities.put(raw, "records",
                                                   identity_key,
                                                   identity_value);
                                });
        expect_invalid_argument("IdentityIndexStore::get",
                                [&identities, raw, &identity_key] {
                                    IdentityIndexValue out;
                                    identities.get(raw, "records", identity_key, out);
                                });
        expect_invalid_argument("IdentityIndexStore::tombstone",
                                [&identities, raw, &identity_key, &identity_value] {
                                    identities.tombstone(raw, "records",
                                                         identity_key,
                                                         identity_value);
                                });
        expect_invalid_argument("IdentityIndexStore::erase",
                                [&identities, raw, &identity_key] {
                                    identities.erase(raw, "records", identity_key);
                                });

        expect_invalid_argument("ThreadLocalChangeAccumulator::record_change",
                                [&accumulator, raw, &bytes] {
                                    accumulator.record_change(raw,
                                                              "records",
                                                              ChangeOpType::Put,
                                                              0,
                                                              bytes,
                                                              bytes);
                                });
        expect_invalid_argument("ThreadLocalChangeAccumulator::flush_in_txn",
                                [&accumulator, raw] {
                                    accumulator.flush_in_txn(raw);
                                });

        foreign.commit();
    }

    {
        mdbxc::Transaction txn =
            conn_a->transaction(mdbxc::TransactionMode::WRITABLE);
        const ApplyOutcome outcome = engine.apply_batch_ex(txn.handle(), batch);
        MDBXC_TEST_ASSERT(outcome.result == ApplyResult::Applied);
        txn.commit();
    }

    conn_a->disconnect();
    conn_b->disconnect();
    cleanup(path_a);
    cleanup(path_b);
}

} // namespace

int main() {
    test_public_tables_reject_foreign_transactions();
    test_sync_components_reject_foreign_transactions();
    return 0;
}
