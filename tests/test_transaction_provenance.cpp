#include <mdbx_containers.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void cleanup(const std::string& p) {
    std::remove(p.c_str());
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path) {
    mdbxc::Config cfg;
    cfg.pathname = path;
    cfg.max_dbs = 32;
    cfg.no_subdir = true;
    return mdbxc::Connection::create(cfg);
}

template<class Fn>
void expect_invalid_argument(const char* name, Fn fn) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected) {
        throw std::runtime_error(
            std::string(name) + " accepted a transaction from another environment");
    }
}

template<class Table, class Key>
bool contains_key(Table& table, const Key& key) {
    return table.contains(key);
}

void test_external_transaction_must_match_table_connection() {
    using namespace mdbxc;
    const std::string path_a = "test_txn_provenance_a.mdbx";
    const std::string path_b = "test_txn_provenance_b.mdbx";
    cleanup(path_a);
    cleanup(path_b);

    auto conn_a = open_env(path_a);
    auto conn_b = open_env(path_b);

    KeyValueTable<int, int> kv(conn_a, "kv");
    KeyTable<int> keys(conn_a, "keys");
    ValueTable<int> value(conn_a, "value");
    SequenceTable<int> seq(conn_a, "seq");
    AnyValueTable<int> any(conn_a, "any");
    KeyMultiValueTable<int, std::string> multi(conn_a, "multi");
    HashedKeyValueStore<std::string, std::string> hashed(conn_a, "hashed");

    auto txn_b = conn_b->transaction(TransactionMode::WRITABLE);

    expect_invalid_argument("KeyValueTable Transaction",
                            [&kv, &txn_b]() { kv.insert_or_assign(1, 10, txn_b); });
    expect_invalid_argument("KeyValueTable raw MDBX_txn",
                            [&kv, &txn_b]() { kv.insert_or_assign(2, 20, txn_b.handle()); });
    expect_invalid_argument("KeyTable Transaction",
                            [&keys, &txn_b]() { keys.insert(1, txn_b); });
    expect_invalid_argument("ValueTable Transaction",
                            [&value, &txn_b]() { value.set(10, txn_b); });
    expect_invalid_argument("SequenceTable Transaction",
                            [&seq, &txn_b]() { seq.insert_or_assign(1, 10, txn_b); });
    expect_invalid_argument("AnyValueTable Transaction",
                            [&any, &txn_b]() { any.set(1, std::string("value"), txn_b); });
    expect_invalid_argument("KeyMultiValueTable Transaction",
                            [&multi, &txn_b]() { multi.insert(1, std::string("value"), txn_b); });
    expect_invalid_argument("HashedKeyValueStore Transaction",
                            [&hashed, &txn_b]() {
                                hashed.insert_or_assign(std::string("k"),
                                                        std::string("v"),
                                                        txn_b);
                            });

    txn_b.commit();

    if (contains_key(kv, 1) || contains_key(kv, 2)) {
        throw std::runtime_error("cross-environment KeyValueTable write reached table A");
    }
    if (!keys.empty() || value.has_value() || seq.count() != 0u ||
        any.contains(1) || multi.count() != 0u || hashed.count() != 0u) {
        throw std::runtime_error("cross-environment write reached a table");
    }

    conn_a->disconnect();
    conn_b->disconnect();
    cleanup(path_a);
    cleanup(path_b);
}

} // namespace

int main() {
    struct Case {
        const char* name;
        void (*fn)();
    };

    const Case cases[] = {
        { "test_external_transaction_must_match_table_connection",
          &test_external_transaction_must_match_table_connection }
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
