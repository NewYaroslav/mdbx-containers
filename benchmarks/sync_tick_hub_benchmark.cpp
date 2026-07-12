/// \file sync_tick_hub_benchmark.cpp
/// \brief Manual benchmark for hub-style sync pull over tick chunks.

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct BenchmarkConfig {
    std::uint64_t origins = 16;
    std::uint64_t historical_chunks_per_origin = 512;
    std::uint64_t new_chunks_per_origin = 8;
    std::uint64_t ticks_per_chunk = 128;
    std::uint64_t max_batches = 64;
};

void cleanup(const std::string& path);

struct CleanupGuard {
    explicit CleanupGuard(const std::string& path_value) : path(path_value) {}
    ~CleanupGuard() { cleanup(path); }

    std::string path;
};

void cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-lck").c_str());
}

std::string benchmark_path() {
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    const std::chrono::nanoseconds ticks =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch());
    return "benchmark_sync_tick_hub_" + std::to_string(ticks.count()) + ".mdbx";
}

std::uint64_t parse_arg(int argc,
                        char** argv,
                        int index,
                        std::uint64_t fallback,
                        const char* name) {
    if (index >= argc) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(argv[index], &end, 10);
    if (end == argv[index] || *end != '\0') {
        throw std::runtime_error(std::string("invalid ") + name + ": " + argv[index]);
    }
    return static_cast<std::uint64_t>(value);
}

BenchmarkConfig parse_config(int argc, char** argv) {
    BenchmarkConfig config;
    config.origins = parse_arg(argc, argv, 1, config.origins, "origins");
    config.historical_chunks_per_origin =
        parse_arg(argc, argv, 2, config.historical_chunks_per_origin,
                  "historical_chunks_per_origin");
    config.new_chunks_per_origin =
        parse_arg(argc, argv, 3, config.new_chunks_per_origin,
                  "new_chunks_per_origin");
    config.ticks_per_chunk =
        parse_arg(argc, argv, 4, config.ticks_per_chunk, "ticks_per_chunk");
    config.max_batches = parse_arg(argc, argv, 5, config.max_batches, "max_batches");
    if (config.origins == 0 || config.max_batches == 0) {
        throw std::runtime_error("origins and max_batches must be positive");
    }
    if (config.ticks_per_chunk > 1000000ULL) {
        throw std::runtime_error("ticks_per_chunk is too large for this benchmark");
    }
    return config;
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path) {
    mdbxc::Config config;
    config.pathname = path;
    config.max_dbs = 16;
    config.no_subdir = true;
    config.size_now = 512LL * 1024LL * 1024LL;
    config.size_upper = 8LL * 1024LL * 1024LL * 1024LL;
    return mdbxc::Connection::create(config);
}

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId node{};
    for (int i = 0; i < 16; ++i) {
        node[i] = static_cast<std::uint8_t>(seed + i);
    }
    return node;
}

mdbxc::sync::NodeId make_origin(std::uint64_t index) {
    mdbxc::sync::NodeId node{};
    node[0] = 0x40;
    for (int i = 0; i < 8; ++i) {
        node[8 + i] = static_cast<std::uint8_t>((index >> ((7 - i) * 8)) & 0xff);
    }
    return node;
}

std::vector<std::uint8_t> make_chunk_key(std::uint64_t origin_index,
                                         std::uint64_t seq) {
    std::vector<std::uint8_t> key(16);
    for (int i = 0; i < 8; ++i) {
        key[i] = static_cast<std::uint8_t>((origin_index >> ((7 - i) * 8)) & 0xff);
        key[8 + i] = static_cast<std::uint8_t>((seq >> ((7 - i) * 8)) & 0xff);
    }
    return key;
}

std::vector<std::uint8_t> make_tick_chunk(std::uint64_t origin_index,
                                          std::uint64_t seq,
                                          std::uint64_t ticks_per_chunk) {
    const std::size_t tick_size = 32;
    const std::size_t size =
        static_cast<std::size_t>(ticks_per_chunk) * tick_size;
    std::vector<std::uint8_t> value(size);
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<std::uint8_t>(
            (origin_index * 131ULL + seq * 17ULL + i) & 0xff);
    }
    return value;
}

mdbxc::sync::ChangeBatch make_tick_batch(std::uint64_t origin_index,
                                         const mdbxc::sync::NodeId& origin,
                                         std::uint64_t seq,
                                         std::uint64_t ticks_per_chunk) {
    mdbxc::sync::ChangeBatch batch;
    batch.origin_node_id = origin;
    batch.seq = seq;
    batch.time_unix_ns = seq;

    mdbxc::sync::ChangeOp op;
    op.op_type = mdbxc::sync::ChangeOpType::Put;
    op.dbi_name = "tick_chunks";
    op.storage_key = make_chunk_key(origin_index, seq);
    op.value = make_tick_chunk(origin_index, seq, ticks_per_chunk);
    batch.ops.push_back(op);
    return batch;
}

double elapsed_ms(std::chrono::steady_clock::time_point start,
                  std::chrono::steady_clock::time_point finish) {
    const std::chrono::microseconds elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start);
    return static_cast<double>(elapsed.count()) / 1000.0;
}

void seed_changelog(const std::shared_ptr<mdbxc::Connection>& conn,
                    const BenchmarkConfig& config) {
    auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
    mdbxc::sync::ChangeLogStore log(conn->env_handle());
    log.open(txn.handle());

    const std::uint64_t total_chunks =
        config.historical_chunks_per_origin + config.new_chunks_per_origin;
    for (std::uint64_t origin_index = 0; origin_index < config.origins; ++origin_index) {
        const mdbxc::sync::NodeId origin = make_origin(origin_index);
        for (std::uint64_t seq = 1; seq <= total_chunks; ++seq) {
            const mdbxc::sync::ChangeBatch batch =
                make_tick_batch(origin_index, origin, seq, config.ticks_per_chunk);
            const std::vector<std::uint8_t> bytes =
                mdbxc::sync::ChangeBatchCodec::encode(batch);
            log.append(txn.handle(), origin, seq, bytes);
        }
    }

    txn.commit();
}

std::uint64_t count_origin_index_entries(
        const std::shared_ptr<mdbxc::Connection>& conn) {
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    mdbxc::sync::OriginIndexStore origins(conn->env_handle());
    if (!origins.open_existing(txn.handle())) {
        return 0;
    }
    const std::vector<mdbxc::sync::NodeId> indexed =
        origins.origins(txn.handle());
    return static_cast<std::uint64_t>(indexed.size());
}

std::uint64_t pull_incremental(mdbxc::sync::SyncEngine& engine,
                               const BenchmarkConfig& config,
                               std::uint64_t& pages) {
    mdbxc::sync::DirectSyncPeer peer(&engine);
    mdbxc::sync::PullRequest request;
    request.requester = make_node(0xB0);
    request.db_id = make_node(0xD0);
    request.max_batches = config.max_batches;
    for (std::uint64_t origin_index = 0; origin_index < config.origins; ++origin_index) {
        request.have.last_seq_by_origin[make_origin(origin_index)] =
            config.historical_chunks_per_origin;
    }

    std::uint64_t pulled = 0;
    pages = 0;
    bool has_more = false;
    do {
        const mdbxc::sync::SyncCursor before = request.have;
        const mdbxc::sync::PullResponse response = peer.pull(request);
        if (!response.ok) {
            throw std::runtime_error("pull failed: " + response.error);
        }
        ++pages;
        pulled += static_cast<std::uint64_t>(response.batches.size());
        for (std::vector<mdbxc::sync::ChangeBatch>::const_iterator it =
                 response.batches.begin();
             it != response.batches.end(); ++it) {
            std::uint64_t& seq = request.have.last_seq_by_origin[it->origin_node_id];
            if (it->seq > seq) {
                seq = it->seq;
            }
        }
        if (response.has_more &&
            request.have.last_seq_by_origin == before.last_seq_by_origin) {
            throw std::runtime_error("pagination made no cursor progress");
        }
        has_more = response.has_more;
    } while (has_more);

    return pulled;
}

int run(int argc, char** argv) {
    const BenchmarkConfig config = parse_config(argc, argv);
    const std::string path = benchmark_path();
    CleanupGuard cleanup_guard(path);
    cleanup(path);

    std::shared_ptr<mdbxc::Connection> conn = open_env(path);
    mdbxc::sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xA0), make_node(0xD0));

    const std::chrono::steady_clock::time_point seed_start =
        std::chrono::steady_clock::now();
    seed_changelog(conn, config);
    const std::chrono::steady_clock::time_point seed_finish =
        std::chrono::steady_clock::now();
    const std::uint64_t origin_index_entries = count_origin_index_entries(conn);
    if (origin_index_entries != config.origins) {
        throw std::runtime_error("wrong number of origin index entries: expected " +
                                 std::to_string(config.origins) + ", got " +
                                 std::to_string(origin_index_entries));
    }

    std::uint64_t pages = 0;
    const std::chrono::steady_clock::time_point pull_start =
        std::chrono::steady_clock::now();
    const std::uint64_t pulled = pull_incremental(engine, config, pages);
    const std::chrono::steady_clock::time_point pull_finish =
        std::chrono::steady_clock::now();

    const std::uint64_t expected = config.origins * config.new_chunks_per_origin;
    if (pulled != expected) {
        throw std::runtime_error("wrong number of pulled batches: expected " +
                                 std::to_string(expected) + ", got " +
                                 std::to_string(pulled));
    }

    conn->disconnect();

    const std::uint64_t seeded =
        config.origins *
        (config.historical_chunks_per_origin + config.new_chunks_per_origin);
    const double seed_ms = elapsed_ms(seed_start, seed_finish);
    const double pull_ms = elapsed_ms(pull_start, pull_finish);
    const double us_per_batch =
        pulled == 0 ? 0.0 : (pull_ms * 1000.0) / static_cast<double>(pulled);

    std::cout << "sync_tick_hub_benchmark\n"
              << "  origins=" << config.origins << "\n"
              << "  historical_chunks_per_origin="
              << config.historical_chunks_per_origin << "\n"
              << "  new_chunks_per_origin=" << config.new_chunks_per_origin << "\n"
              << "  ticks_per_chunk=" << config.ticks_per_chunk << "\n"
              << "  max_batches=" << config.max_batches << "\n"
              << "  seeded_batches=" << seeded << "\n"
              << "  origin_index_entries=" << origin_index_entries << "\n"
              << "  pulled_batches=" << pulled << "\n"
              << "  pull_pages=" << pages << "\n"
              << "  seed_ms=" << seed_ms << "\n"
              << "  pull_ms=" << pull_ms << "\n"
              << "  pull_us_per_batch=" << us_per_batch << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "FAIL sync_tick_hub_benchmark: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "FAIL sync_tick_hub_benchmark: non-std exception\n";
    }
    return 1;
}
