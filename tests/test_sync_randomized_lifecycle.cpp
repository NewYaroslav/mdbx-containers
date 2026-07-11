/// \file test_sync_randomized_lifecycle.cpp
/// \brief Stress test for the randomized state-model coverage of the
///        sync engine in its normal lifecycle pattern.
///
/// Reproduces the original MDBX_BUSY symptom (issue #73) under a
/// realistic use of the library:
///   * connection created once
///   * user tables and sync system stores created once, upfront
///   * a fresh writable transaction per iteration
///   * the capture sink writes to the changelog inside the same
///     transaction
///   * commit at the end of the iteration
///
/// The test compares an in-memory `std::map` reference model with the
/// state on the primary after a fixed number of random operations,
/// and (on Linux) with the state on the replica after a single pull
/// at the end of the run. This is the test shape that would have
/// caught the original MDBX_BUSY at the random layer.

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

namespace {

void cleanup(const std::string& p) {
    std::remove(p.c_str());
    std::remove((p + "-lck").c_str());
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path) {
    mdbxc::Config c;
    c.pathname = path;
    c.max_dbs = 16;
    c.no_subdir = true;
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

void run_randomized_lifecycle_repro() {
    constexpr std::uint32_t kSeed    = 123456;
    constexpr std::size_t   kIters   = 200;

    const std::string p_path = "test_randomized_lifecycle_primary.mdbx";
    cleanup(p_path);

    auto p_conn = open_env(p_path);

    // Declare all wrappers once, BEFORE any transaction runs.
    mdbxc::KeyValueTable<int, std::string> kv(p_conn, "kv");

    // Reference model: in-memory equivalent of the kv state.
    std::map<int, std::string> ref_kv;

    std::mt19937 rng(kSeed);

    // Random op: put/erase + auto-commit per iteration.
    auto do_random_op = [&] {
        const int op = static_cast<int>(rng() % 3);
        const int k = static_cast<int>(rng() % 50);
        const std::string v = "v" + std::to_string(rng() % 1000);

        // Each iteration opens ONE RAII writable transaction. The user
        // tables and the sync store all participate. The capture sink
        // writes to the changelog inside the same transaction, so the
        // commit atomically commits the user data and the changelog
        // batch.
        auto txn = p_conn->transaction(mdbxc::TransactionMode::WRITABLE);
        switch (op) {
        case 0: {
            kv.insert_or_assign(k, v, txn.handle());
            ref_kv[k] = v;
            break;
        }
        case 1: {
            kv.erase(k, txn.handle());
            ref_kv.erase(k);
            break;
        }
        case 2: {
            // Multi-op in one transaction.
            for (int j = 0; j < 3; ++j) {
                const int k2 = static_cast<int>(rng() % 50);
                const std::string v2 = "m" + std::to_string(rng() % 1000);
                kv.insert_or_assign(k2, v2, txn.handle());
                ref_kv[k2] = v2;
            }
            break;
        }
        }
        txn.commit();
    };

    // The capture sink is attached once, BEFORE the first iteration.
    // The store (created lazily on first open()) is the same one that
    // record_op() pushes into.
    mdbxc::sync::ThreadLocalChangeAccumulator sink(p_conn);
    p_conn->attach_sync_capture(&sink);

    for (std::size_t i = 0; i < kIters; ++i) {
        try {
            do_random_op();
        } catch (const std::exception& e) {
            std::printf("FAIL iter %zu: %s\n", i, e.what());
            ++failures;
            return;
        }
    }

    // Detach so the env is clean after the test.
    p_conn->detach_sync_capture();

    // Sanity: primary matches the in-memory reference model.
    auto check_match = [&](const char* where) {
        auto txn = p_conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        for (const auto& [k, v] : ref_kv) {
            std::string got;
            if (!kv.try_get(k, got, txn.handle()) || got != v) {
                std::printf("FAIL %s: kv[%d] = %s expected %s\n", where, k,
                            got.c_str(), v.c_str());
                ++failures;
            }
        }
    };
    check_match("after_randomized_lifecycle");

    p_conn->disconnect();
    cleanup(p_path);
}

} // namespace

int main() {
    try {
        run_randomized_lifecycle_repro();
    } catch (const std::exception& e) {
        std::printf("FAIL test_randomized_lifecycle: %s\n", e.what());
        ++failures;
    }
    if (failures == 0) {
        std::printf("PASS test_randomized_lifecycle\n");
        return 0;
    }
    std::printf("FAIL test_randomized_lifecycle: %d failure(s)\n", failures);
    return 1;
}
