/// \file test_txn_overlap.cpp
/// \brief Regression tests for the BaseTable / Connection transaction
///        lifecycle that previously caused MDBX_BUSY when a table was
///        opened inside an active write transaction.
///
/// Two scenarios covered:
///   1. Table construction inside an outer write transaction reuses
///      the outer transaction (no nested write txn, no MDBX_BUSY).
///   2. The ctor of a read-only connection can run alongside any
///      other connection's write activity.

#include <mdbx_containers.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void cleanup(const std::string& p) {
    std::remove(p.c_str());
    std::remove((p + "-lck").c_str());
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path,
                                          bool read_only = false) {
    mdbxc::Config c;
    c.pathname = path;
    c.max_dbs = 8;
    c.no_subdir = true;
    c.read_only = read_only;
    return mdbxc::Connection::create(c);
}

int failures = 0;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

void test_table_ctor_inside_outer_write_txn() {
    const std::string p = "test_txn_overlap_ctor.mdbx";
    cleanup(p);

    auto conn = open_env(p);
    {
        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        // The ctor used to open its own write transaction here, which
        // collided with the outer one and produced MDBX_BUSY. After the
        // fix, with_transaction reuses the outer transaction.
        mdbxc::KeyValueTable<int, std::string> kv(conn, "kv");
        kv.insert_or_assign(1, "one", txn.handle());
        txn.commit();
    }
    auto conn2 = open_env(p);
    mdbxc::KeyValueTable<int, std::string> kv(conn2, "kv");
    mdbxc::KeyValueTable<int, std::string> empty(conn2, "empty");
    {
        auto txn = conn2->transaction(mdbxc::TransactionMode::READ_ONLY);
        std::string got;
        CHECK(kv.try_get(1, got, txn.handle()));
        CHECK(got == "one");
    }
}

void test_read_only_ctor_alongside_other_write() {
    const std::string write_path = "test_txn_overlap_write.mdbx";
    const std::string read_path  = "test_txn_overlap_read.mdbx";
    cleanup(write_path);
    cleanup(read_path);

    // Open the write connection, create a table there, write, do not
    // close yet. (No active write transaction: the ctor committed.)
    auto wconn = open_env(write_path);
    mdbxc::KeyValueTable<int, std::string> wkv(wconn, "kv");
    wkv.insert_or_assign(42, "write");

    // Now open a read-only connection in a different env file. The ctor
    // uses a READ_ONLY transaction; no contention with the write env.
    auto rconn = open_env(read_path, /*read_only=*/true);
    mdbxc::KeyValueTable<int, std::string> rkv(rconn, "kv");
    std::string got;
    CHECK(!rkv.try_get(42, got));
}

} // namespace

int main() {
    try { test_table_ctor_inside_outer_write_txn(); }
    catch (const std::exception& e) {
        std::printf("EXCEPTION: %s\n", e.what());
        ++failures;
    }
    try { test_read_only_ctor_alongside_other_write(); }
    catch (const std::exception& e) {
        std::printf("EXCEPTION: %s\n", e.what());
        ++failures;
    }
    if (failures == 0) {
        std::printf("PASS test_txn_overlap\n");
        return 0;
    }
    std::printf("FAIL test_txn_overlap: %d failure(s)\n", failures);
    return 1;
}
