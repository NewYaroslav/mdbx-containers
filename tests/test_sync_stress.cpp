/// \file test_sync_stress.cpp
/// \brief Longer fixed-seed sync stress test with pagination and restarts.

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

typedef std::map<int, std::string> Model;

void cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-lck").c_str());
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path) {
    mdbxc::Config config;
    config.pathname = path;
    config.max_dbs = 16;
    config.no_subdir = true;
    return mdbxc::Connection::create(config);
}

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId node{};
    for (int i = 0; i < 16; ++i) {
        node[i] = static_cast<std::uint8_t>(seed + i);
    }
    return node;
}

void require_table_matches_model(const std::shared_ptr<mdbxc::Connection>& conn,
                                 mdbxc::KeyValueTable<int, std::string>& table,
                                 const Model& expected,
                                 const char* label) {
    Model actual;
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    table.load(actual, txn.handle());
    txn.commit();
    if (actual != expected) {
        throw std::runtime_error(std::string(label) + " does not match reference model");
    }
}

class StressHarness {
public:
    StressHarness(std::uint32_t seed,
                  const mdbxc::sync::NodeId& primary_node,
                  const mdbxc::sync::NodeId& replica_node,
                  const mdbxc::sync::NodeId& db_id)
        : m_primary_path("test_sync_stress_primary_" + std::to_string(seed) + ".mdbx"),
          m_replica_path("test_sync_stress_replica_" + std::to_string(seed) + ".mdbx"),
          m_primary_node(primary_node),
          m_replica_node(replica_node),
          m_db_id(db_id),
          m_capture_attached(false) {}

    ~StressHarness() {
        try {
            close();
        } catch (...) {}
        cleanup(m_primary_path);
        cleanup(m_replica_path);
    }

    void reset_files() {
        cleanup(m_primary_path);
        cleanup(m_replica_path);
    }

    void open() {
        m_primary_conn = open_env(m_primary_path);
        m_replica_conn = open_env(m_replica_path);

        m_primary_engine.reset(new mdbxc::sync::SyncEngine(m_primary_conn));
        m_replica_engine.reset(new mdbxc::sync::SyncEngine(m_replica_conn));
        m_primary_engine->initialize_local_identity(m_primary_node, m_db_id);
        m_replica_engine->initialize_local_identity(m_replica_node, m_db_id);

        m_primary_kv.reset(
            new mdbxc::KeyValueTable<int, std::string>(m_primary_conn, "kv"));
        m_replica_kv.reset(
            new mdbxc::KeyValueTable<int, std::string>(m_replica_conn, "kv"));

        m_sink.reset(new mdbxc::sync::ThreadLocalChangeAccumulator(m_primary_conn));
        m_primary_conn->attach_sync_capture(m_sink.get());
        m_capture_attached = true;
    }

    void close() {
        if (m_capture_attached && m_primary_conn) {
            m_primary_conn->detach_sync_capture();
            m_capture_attached = false;
        }
        m_sink.reset();
        m_primary_kv.reset();
        m_replica_kv.reset();
        m_primary_engine.reset();
        m_replica_engine.reset();
        if (m_primary_conn) {
            m_primary_conn->disconnect();
            m_primary_conn.reset();
        }
        if (m_replica_conn) {
            m_replica_conn->disconnect();
            m_replica_conn.reset();
        }
    }

    void restart() {
        close();
        open();
    }

    void require_primary_matches(const Model& expected, const char* label) {
        require_table_matches_model(m_primary_conn, *m_primary_kv, expected, label);
    }

    void require_replica_matches(const Model& expected, const char* label) {
        require_table_matches_model(m_replica_conn, *m_replica_kv, expected, label);
    }

    void sync_to_replica(std::uint64_t max_batches) {
        mdbxc::sync::DirectSyncPeer peer(m_primary_engine.get());
        mdbxc::sync::PullRequest request;
        request.requester = m_replica_node;
        request.db_id = m_db_id;
        request.have = m_replica_engine->applied_cursor();
        request.max_batches = max_batches;

        bool has_more = false;
        do {
            const mdbxc::sync::SyncCursor before = request.have;
            const mdbxc::sync::PullResponse response = peer.pull(request);
            if (!response.ok) {
                throw std::runtime_error("stress pull failed: " + response.error);
            }
            if (!response.batches.empty()) {
                auto txn = m_replica_conn->transaction(mdbxc::TransactionMode::WRITABLE);
                for (std::vector<mdbxc::sync::ChangeBatch>::const_iterator it =
                         response.batches.begin();
                     it != response.batches.end(); ++it) {
                    const mdbxc::sync::ApplyResult result =
                        m_replica_engine->apply_batch(txn.handle(), *it);
                    if (result == mdbxc::sync::ApplyResult::Conflict) {
                        throw std::runtime_error("stress replica apply returned Conflict");
                    }
                }
                txn.commit();
            } else if (response.has_more) {
                throw std::runtime_error("stress pull reported has_more without batches");
            }

            has_more = response.has_more;
            request.have = m_replica_engine->applied_cursor();
            if (has_more &&
                request.have.last_seq_by_origin == before.last_seq_by_origin) {
                throw std::runtime_error("stress pull pagination made no cursor progress");
            }
        } while (has_more);
    }

    std::shared_ptr<mdbxc::Connection> primary_conn() const {
        return m_primary_conn;
    }

    mdbxc::KeyValueTable<int, std::string>& primary_table() {
        return *m_primary_kv;
    }

private:
    std::string m_primary_path;
    std::string m_replica_path;
    mdbxc::sync::NodeId m_primary_node;
    mdbxc::sync::NodeId m_replica_node;
    mdbxc::sync::NodeId m_db_id;
    std::shared_ptr<mdbxc::Connection> m_primary_conn;
    std::shared_ptr<mdbxc::Connection> m_replica_conn;
    std::unique_ptr<mdbxc::sync::SyncEngine> m_primary_engine;
    std::unique_ptr<mdbxc::sync::SyncEngine> m_replica_engine;
    std::unique_ptr<mdbxc::KeyValueTable<int, std::string> > m_primary_kv;
    std::unique_ptr<mdbxc::KeyValueTable<int, std::string> > m_replica_kv;
    std::unique_ptr<mdbxc::sync::ThreadLocalChangeAccumulator> m_sink;
    bool m_capture_attached;
};

void apply_random_operation(StressHarness& harness,
                            Model& model,
                            std::mt19937& rng) {
    const int op = static_cast<int>(rng() % 8);
    const int key = static_cast<int>(rng() % 512);
    const std::string value = "v" + std::to_string(rng() % 100000);

    auto txn = harness.primary_conn()->transaction(mdbxc::TransactionMode::WRITABLE);
    switch (op) {
    case 0:
    case 1:
    case 2:
        harness.primary_table().insert_or_assign(key, value, txn.handle());
        model[key] = value;
        txn.commit();
        return;
    case 3:
        harness.primary_table().erase(key, txn.handle());
        model.erase(key);
        txn.commit();
        return;
    case 4:
        if ((rng() % 11) == 0) {
            harness.primary_table().clear(txn.handle());
            model.clear();
        } else {
            harness.primary_table().erase(key, txn.handle());
            model.erase(key);
        }
        txn.commit();
        return;
    case 5:
        harness.primary_table().insert_or_assign(key, value, txn.handle());
        txn.rollback();
        return;
    default:
        for (int i = 0; i < 4; ++i) {
            const int multi_key = static_cast<int>(rng() % 512);
            const std::string multi_value = "m" + std::to_string(rng() % 100000);
            harness.primary_table().insert_or_assign(multi_key, multi_value, txn.handle());
            model[multi_key] = multi_value;
        }
        txn.commit();
        return;
    }
}

void run_seed(std::uint32_t seed) {
    const std::size_t operations = 10000;
    const std::size_t sync_every = 250;
    const std::size_t restart_every = 2500;
    const std::uint64_t max_batches_per_pull = 64;

    StressHarness harness(seed, make_node(0xA0), make_node(0xB0), make_node(0xD0));
    harness.reset_files();
    harness.open();

    Model reference;
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < operations; ++i) {
        apply_random_operation(harness, reference, rng);

        if ((i + 1) % restart_every == 0) {
            harness.restart();
            harness.require_primary_matches(reference, "stress primary after restart");
            harness.sync_to_replica(max_batches_per_pull);
            harness.require_replica_matches(reference, "stress replica after restart sync");
        } else if ((i + 1) % sync_every == 0) {
            harness.require_primary_matches(reference, "stress primary");
            harness.sync_to_replica(max_batches_per_pull);
            harness.require_replica_matches(reference, "stress replica");
        }
    }

    harness.require_primary_matches(reference, "stress primary final");
    harness.sync_to_replica(max_batches_per_pull);
    harness.require_replica_matches(reference, "stress replica final");
    harness.close();
    harness.reset_files();
}

void run_stress() {
    const std::uint32_t seeds[] = { 123456u, 987654u, 424242u };
    for (std::size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        run_seed(seeds[i]);
    }
}

} // namespace

int main() {
    try {
        run_stress();
    } catch (const std::exception& e) {
        std::printf("FAIL test_sync_stress: %s\n", e.what());
        return 1;
    } catch (...) {
        std::printf("FAIL test_sync_stress: non-std exception\n");
        return 1;
    }

    std::printf("PASS test_sync_stress\n");
    return 0;
}
