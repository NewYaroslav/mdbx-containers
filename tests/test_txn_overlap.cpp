/// \file test_txn_overlap.cpp
/// \brief Minimal repro for MDBX_TXN_OVERLAPPING and MDBX_BUSY issues
///        found while writing the randomized state-model test.

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void cleanup(const std::string& p) {
    std::remove(p.c_str());
    std::remove((p + "-lck").c_str());
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path) {
    using namespace mdbxc;
    Config c;
    c.pathname = path;
    c.max_dbs = 4;
    c.no_subdir = true;
    return Connection::create(c);
}

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId n{};
    for (int i = 0; i < 16; ++i) n[i] = static_cast<std::uint8_t>(seed + i);
    return n;
}

int run(const char* scenario, std::size_t iters, bool with_sink) {
    std::printf("\n=== %s (sink=%d, %zu iters) ===\n", scenario,
                with_sink ? 1 : 0, iters);
    const std::string p_path = "test_txn_overlap_p.mdbx";
    const std::string r_path = "test_txn_overlap_r.mdbx";
    cleanup(p_path); cleanup(r_path);

    auto p = open_env(p_path);
    auto r = open_env(r_path);
    mdbxc::sync::SyncEngine pe(p), re(r);
    pe.initialize_local_identity(make_node(0xA0), make_node(0xD0));
    re.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    mdbxc::sync::ThreadLocalChangeAccumulator sink(p);
    if (with_sink) {
        p->attach_sync_capture(&sink);
    }

    int fail_at = -1;
    for (std::size_t i = 0; i < iters; ++i) {
        try {
            {
                auto txn = p->transaction(mdbxc::TransactionMode::WRITABLE);
                // Simulate a kv insert that triggers sink->record_change.
                mdbxc::KeyValueTable<int, std::string> kv(p, "kv");
                kv.insert_or_assign(static_cast<int>(i), "v" + std::to_string(i));
                txn.commit();
            }
            {
                auto txn = p->transaction(mdbxc::TransactionMode::READ_ONLY);
            }
        } catch (const std::exception& e) {
            std::printf("[%s] iter %zu EXCEPTION: %s\n", scenario, i, e.what());
            fail_at = static_cast<int>(i);
            break;
        }
    }
    if (with_sink) p->detach_sync_capture();
    p->disconnect();
    r->disconnect();
    cleanup(p_path);
    cleanup(r_path);
    if (fail_at < 0) {
        std::printf("[%s] OK\n", scenario);
    }
    return fail_at < 0 ? 0 : 1;
}

} // namespace

int main() {
    {
        const std::string p_path = "test_simple_p.mdbx";
        cleanup(p_path);
        auto p = open_env(p_path);
        mdbxc::sync::SyncEngine pe(p);
        pe.initialize_local_identity(make_node(0xA0), make_node(0xD0));
        try {
            auto txn = p->transaction(mdbxc::TransactionMode::WRITABLE);
            txn.commit();
            std::printf("[pe-only-init] first WRITABLE OK\n");
        } catch (const std::exception& e) {
            std::printf("[pe-only-init] FAIL: %s\n", e.what());
        }
        p->disconnect();
        cleanup(p_path);
    }
    return 0;
}
