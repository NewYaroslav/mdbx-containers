/// \file sync_tick_hub_benchmark.cpp
/// \brief Manual benchmark for hub-style sync pull/apply over tick chunks.

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const std::uint64_t default_max_bytes = 4ULL * 1024ULL * 1024ULL;

struct Scenario {
    std::string   name;
    std::uint64_t origins;
    std::uint64_t historical_chunks_per_origin;
    std::uint64_t new_chunks_per_origin;
    std::uint64_t ticks_per_chunk;
    std::uint64_t max_batches;
    std::uint64_t max_bytes;
};

struct Paths {
    std::string primary;
    std::string replica;
};

struct PullApplyMetrics {
    std::uint64_t pulled_batches = 0;
    std::uint64_t applied_batches = 0;
    std::uint64_t pull_pages = 0;
    double pull_ms = 0.0;
    double apply_ms = 0.0;
};

struct PhaseMetrics {
    std::string   phase;
    std::uint64_t chunks_per_origin = 0;
    std::uint64_t seeded_batches = 0;
    std::uint64_t pulled_batches = 0;
    std::uint64_t applied_batches = 0;
    std::uint64_t pull_pages = 0;
    double        seed_ms = 0.0;
    double        restart_ms = 0.0;
    double        pull_ms = 0.0;
    double        apply_ms = 0.0;
    std::uint64_t primary_bytes = 0;
    std::uint64_t replica_bytes = 0;
};

void cleanup(const std::string& path);

struct CleanupGuard {
    explicit CleanupGuard(const Paths& paths_value) : paths(paths_value) {}

    ~CleanupGuard() {
        cleanup(paths.primary);
        cleanup(paths.replica);
    }

    Paths paths;
};

void cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-lck").c_str());
}

std::string run_id() {
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    const std::chrono::nanoseconds ticks =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch());
    return std::to_string(ticks.count());
}

Paths make_paths(const std::string& id, const std::string& scenario_name) {
    Paths paths;
    const std::string prefix = "benchmark_sync_tick_hub_" + id + "_" + scenario_name;
    paths.primary = prefix + "_primary.mdbx";
    paths.replica = prefix + "_replica.mdbx";
    return paths;
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

void validate_scenario(const Scenario& scenario) {
    if (scenario.origins == 0 ||
        scenario.max_batches == 0 ||
        scenario.max_bytes == 0) {
        throw std::runtime_error("origins, max_batches, and max_bytes must be positive");
    }
    if (scenario.historical_chunks_per_origin == 0 &&
        scenario.new_chunks_per_origin == 0) {
        throw std::runtime_error(
            "historical_chunks_per_origin and new_chunks_per_origin cannot both be zero");
    }
    if (scenario.ticks_per_chunk > 1000000ULL) {
        throw std::runtime_error("ticks_per_chunk is too large for this benchmark");
    }
}

std::vector<Scenario> default_scenarios() {
    std::vector<Scenario> scenarios;

    Scenario single;
    single.name = "one_origin_small_chunks";
    single.origins = 1;
    single.historical_chunks_per_origin = 512;
    single.new_chunks_per_origin = 64;
    single.ticks_per_chunk = 16;
    single.max_batches = 64;
    single.max_bytes = default_max_bytes;
    scenarios.push_back(single);

    Scenario hub;
    hub.name = "ten_origins_tick_chunks";
    hub.origins = 10;
    hub.historical_chunks_per_origin = 512;
    hub.new_chunks_per_origin = 32;
    hub.ticks_per_chunk = 128;
    hub.max_batches = 64;
    hub.max_bytes = default_max_bytes;
    scenarios.push_back(hub);

    Scenario paged;
    paged.name = "hundred_origins_paged";
    paged.origins = 100;
    paged.historical_chunks_per_origin = 128;
    paged.new_chunks_per_origin = 8;
    paged.ticks_per_chunk = 128;
    paged.max_batches = 32;
    paged.max_bytes = default_max_bytes;
    scenarios.push_back(paged);

    return scenarios;
}

std::vector<Scenario> parse_scenarios(int argc, char** argv) {
    if (argc == 1) {
        return default_scenarios();
    }
    if (std::string(argv[1]) == "--help" ||
        std::string(argv[1]) == "-h") {
        std::cout
            << "usage: sync_tick_hub_benchmark "
            << "[origins historical_chunks_per_origin new_chunks_per_origin "
            << "ticks_per_chunk max_batches max_bytes]\n"
            << "\n"
            << "Without arguments, runs a small built-in scenario matrix.\n"
            << "The new_chunks_per_origin value is used for both hot and "
            << "after-restart incremental phases.\n";
        std::exit(0);
    }

    Scenario scenario;
    scenario.name = "custom";
    scenario.origins = parse_arg(argc, argv, 1, 16, "origins");
    scenario.historical_chunks_per_origin =
        parse_arg(argc, argv, 2, 512, "historical_chunks_per_origin");
    scenario.new_chunks_per_origin =
        parse_arg(argc, argv, 3, 8, "new_chunks_per_origin");
    scenario.ticks_per_chunk =
        parse_arg(argc, argv, 4, 128, "ticks_per_chunk");
    scenario.max_batches = parse_arg(argc, argv, 5, 64, "max_batches");
    scenario.max_bytes = parse_arg(argc, argv, 6, default_max_bytes, "max_bytes");
    validate_scenario(scenario);

    std::vector<Scenario> scenarios;
    scenarios.push_back(scenario);
    return scenarios;
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path) {
    mdbxc::Config config;
    config.pathname = path;
    config.max_dbs = 16;
    config.no_subdir = true;
    config.size_now = 512LL * 1024LL * 1024LL;
    config.size_upper = 16LL * 1024LL * 1024LL * 1024LL;
    return mdbxc::Connection::create(config);
}

std::uint64_t used_database_bytes(
        const std::shared_ptr<mdbxc::Connection>& conn) {
    MDBX_envinfo info;
    std::memset(&info, 0, sizeof(info));
    mdbxc::check_mdbx(
        mdbx_env_info_ex(conn->env_handle(), nullptr, &info, sizeof(info)),
        "sync_tick_hub_benchmark: failed to read MDBX env info");
    return (info.mi_last_pgno + 1) *
           static_cast<std::uint64_t>(info.mi_dxb_pagesize);
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

std::uint64_t append_changelog_range(
        const std::shared_ptr<mdbxc::Connection>& conn,
        const Scenario& scenario,
        std::uint64_t first_seq,
        std::uint64_t chunks_per_origin) {
    if (chunks_per_origin == 0) {
        return 0;
    }

    auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
    mdbxc::sync::ChangeLogStore log(conn->env_handle());
    log.open(txn.handle());

    for (std::uint64_t origin_index = 0; origin_index < scenario.origins; ++origin_index) {
        const mdbxc::sync::NodeId origin = make_origin(origin_index);
        const std::uint64_t end_seq = first_seq + chunks_per_origin;
        for (std::uint64_t seq = first_seq; seq < end_seq; ++seq) {
            const mdbxc::sync::ChangeBatch batch =
                make_tick_batch(origin_index, origin, seq, scenario.ticks_per_chunk);
            const std::vector<std::uint8_t> bytes =
                mdbxc::sync::ChangeBatchCodec::encode(batch);
            log.append(txn.handle(), origin, seq, bytes);
        }
    }

    txn.commit();
    return scenario.origins * chunks_per_origin;
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

void verify_origin_index(const std::shared_ptr<mdbxc::Connection>& conn,
                         const Scenario& scenario,
                         std::uint64_t expected_origins) {
    const std::uint64_t actual = count_origin_index_entries(conn);
    if (actual != expected_origins) {
        throw std::runtime_error("wrong number of origin index entries: expected " +
                                 std::to_string(expected_origins) + ", got " +
                                 std::to_string(actual) + " for scenario " +
                                 scenario.name);
    }
}

PullApplyMetrics pull_and_apply(mdbxc::sync::SyncEngine& primary_engine,
                                mdbxc::sync::SyncEngine& replica_engine,
                                const Scenario& scenario,
                                const mdbxc::sync::NodeId& replica_node,
                                const mdbxc::sync::NodeId& db_id) {
    mdbxc::sync::DirectSyncPeer peer(&primary_engine);
    mdbxc::sync::PullRequest request;
    request.requester = replica_node;
    request.db_id = db_id;
    request.have = replica_engine.applied_cursor();
    request.max_batches = scenario.max_batches;
    request.max_bytes = scenario.max_bytes;

    PullApplyMetrics metrics;
    bool has_more = false;
    do {
        const mdbxc::sync::SyncCursor before = request.have;
        const std::chrono::steady_clock::time_point pull_start =
            std::chrono::steady_clock::now();
        const mdbxc::sync::PullResponse response = peer.pull(request);
        const std::chrono::steady_clock::time_point pull_finish =
            std::chrono::steady_clock::now();
        metrics.pull_ms += elapsed_ms(pull_start, pull_finish);

        if (!response.ok) {
            throw std::runtime_error("pull failed: " + response.error);
        }
        ++metrics.pull_pages;
        metrics.pulled_batches += static_cast<std::uint64_t>(response.batches.size());

        if (!response.batches.empty()) {
            mdbxc::sync::PushRequest push;
            push.sender = primary_engine.local_node_id();
            push.db_id = db_id;
            push.batches = response.batches;

            const std::chrono::steady_clock::time_point apply_start =
                std::chrono::steady_clock::now();
            const mdbxc::sync::PushResponse pushed =
                replica_engine.handle_push(push);
            const std::chrono::steady_clock::time_point apply_finish =
                std::chrono::steady_clock::now();
            metrics.apply_ms += elapsed_ms(apply_start, apply_finish);

            if (!pushed.ok) {
                throw std::runtime_error("apply failed: " + pushed.error);
            }
            metrics.applied_batches +=
                static_cast<std::uint64_t>(response.batches.size());
        } else if (response.has_more) {
            throw std::runtime_error("pull reported has_more without batches");
        }

        has_more = response.has_more;
        request.have = replica_engine.applied_cursor();
        if (has_more &&
            request.have.last_seq_by_origin == before.last_seq_by_origin) {
            throw std::runtime_error("pull pagination made no cursor progress");
        }
    } while (has_more);

    return metrics;
}

PhaseMetrics run_sync_phase(const std::string& phase,
                            std::uint64_t chunks_per_origin,
                            std::uint64_t seeded_batches,
                            double seed_ms,
                            double restart_ms,
                            const std::shared_ptr<mdbxc::Connection>& primary_conn,
                            const std::shared_ptr<mdbxc::Connection>& replica_conn,
                            mdbxc::sync::SyncEngine& primary_engine,
                            mdbxc::sync::SyncEngine& replica_engine,
                            const Scenario& scenario,
                            const mdbxc::sync::NodeId& replica_node,
                            const mdbxc::sync::NodeId& db_id) {
    const PullApplyMetrics sync =
        pull_and_apply(primary_engine, replica_engine, scenario, replica_node, db_id);
    const std::uint64_t expected = scenario.origins * chunks_per_origin;
    if (sync.pulled_batches != expected) {
        throw std::runtime_error(phase + " pulled wrong batch count: expected " +
                                 std::to_string(expected) + ", got " +
                                 std::to_string(sync.pulled_batches));
    }
    if (sync.applied_batches != expected) {
        throw std::runtime_error(phase + " applied wrong batch count: expected " +
                                 std::to_string(expected) + ", got " +
                                 std::to_string(sync.applied_batches));
    }

    PhaseMetrics metrics;
    metrics.phase = phase;
    metrics.chunks_per_origin = chunks_per_origin;
    metrics.seeded_batches = seeded_batches;
    metrics.pulled_batches = sync.pulled_batches;
    metrics.applied_batches = sync.applied_batches;
    metrics.pull_pages = sync.pull_pages;
    metrics.seed_ms = seed_ms;
    metrics.restart_ms = restart_ms;
    metrics.pull_ms = sync.pull_ms;
    metrics.apply_ms = sync.apply_ms;
    metrics.primary_bytes = used_database_bytes(primary_conn);
    metrics.replica_bytes = used_database_bytes(replica_conn);
    return metrics;
}

void initialize_engine(mdbxc::sync::SyncEngine& engine,
                       const mdbxc::sync::NodeId& node,
                       const mdbxc::sync::NodeId& db_id) {
    engine.initialize_local_identity(node, db_id);
}

void print_csv_header() {
    std::cout
        << "scenario,phase,origins,historical_chunks_per_origin,"
        << "new_chunks_per_origin,chunks_per_origin,ticks_per_chunk,"
        << "max_batches,max_bytes,seeded_batches,pulled_batches,"
        << "applied_batches,pull_pages,seed_ms,restart_ms,pull_ms,"
        << "apply_ms,total_ms,batches_per_sec,primary_bytes,replica_bytes\n";
}

void print_csv_row(const Scenario& scenario,
                   const PhaseMetrics& metrics) {
    const double total_ms =
        metrics.seed_ms + metrics.restart_ms + metrics.pull_ms + metrics.apply_ms;
    const double batches_per_sec =
        (metrics.pull_ms + metrics.apply_ms) <= 0.0
            ? 0.0
            : (static_cast<double>(metrics.applied_batches) * 1000.0) /
              (metrics.pull_ms + metrics.apply_ms);
    std::cout << scenario.name << ','
              << metrics.phase << ','
              << scenario.origins << ','
              << scenario.historical_chunks_per_origin << ','
              << scenario.new_chunks_per_origin << ','
              << metrics.chunks_per_origin << ','
              << scenario.ticks_per_chunk << ','
              << scenario.max_batches << ','
              << scenario.max_bytes << ','
              << metrics.seeded_batches << ','
              << metrics.pulled_batches << ','
              << metrics.applied_batches << ','
              << metrics.pull_pages << ','
              << metrics.seed_ms << ','
              << metrics.restart_ms << ','
              << metrics.pull_ms << ','
              << metrics.apply_ms << ','
              << total_ms << ','
              << batches_per_sec << ','
              << metrics.primary_bytes << ','
              << metrics.replica_bytes << '\n';
}

void run_scenario(const Scenario& scenario, const std::string& id) {
    validate_scenario(scenario);
    const mdbxc::sync::NodeId primary_node = make_node(0xA0);
    const mdbxc::sync::NodeId replica_node = make_node(0xB0);
    const mdbxc::sync::NodeId db_id = make_node(0xD0);
    const Paths paths = make_paths(id, scenario.name);
    CleanupGuard cleanup_guard(paths);
    cleanup(paths.primary);
    cleanup(paths.replica);

    std::shared_ptr<mdbxc::Connection> primary_conn = open_env(paths.primary);
    std::shared_ptr<mdbxc::Connection> replica_conn = open_env(paths.replica);

    {
        mdbxc::sync::SyncEngine primary_engine(primary_conn);
        mdbxc::sync::SyncEngine replica_engine(replica_conn);
        initialize_engine(primary_engine, primary_node, db_id);
        initialize_engine(replica_engine, replica_node, db_id);

        const std::chrono::steady_clock::time_point historical_seed_start =
            std::chrono::steady_clock::now();
        const std::uint64_t historical_seeded =
            append_changelog_range(primary_conn, scenario, 1,
                                   scenario.historical_chunks_per_origin);
        const std::chrono::steady_clock::time_point historical_seed_finish =
            std::chrono::steady_clock::now();
        verify_origin_index(primary_conn, scenario,
                            scenario.historical_chunks_per_origin == 0
                                ? 0
                                : scenario.origins);

        PhaseMetrics full = run_sync_phase(
            "full_cold_replica",
            scenario.historical_chunks_per_origin,
            historical_seeded,
            elapsed_ms(historical_seed_start, historical_seed_finish),
            0.0,
            primary_conn,
            replica_conn,
            primary_engine,
            replica_engine,
            scenario,
            replica_node,
            db_id);
        print_csv_row(scenario, full);

        const std::uint64_t hot_first_seq = scenario.historical_chunks_per_origin + 1;
        const std::chrono::steady_clock::time_point hot_seed_start =
            std::chrono::steady_clock::now();
        const std::uint64_t hot_seeded =
            append_changelog_range(primary_conn, scenario, hot_first_seq,
                                   scenario.new_chunks_per_origin);
        const std::chrono::steady_clock::time_point hot_seed_finish =
            std::chrono::steady_clock::now();
        verify_origin_index(primary_conn, scenario, scenario.origins);

        PhaseMetrics hot = run_sync_phase(
            "incremental_hot",
            scenario.new_chunks_per_origin,
            hot_seeded,
            elapsed_ms(hot_seed_start, hot_seed_finish),
            0.0,
            primary_conn,
            replica_conn,
            primary_engine,
            replica_engine,
            scenario,
            replica_node,
            db_id);
        print_csv_row(scenario, hot);
    }

    const std::chrono::steady_clock::time_point restart_start =
        std::chrono::steady_clock::now();
    primary_conn->disconnect();
    replica_conn->disconnect();
    primary_conn.reset();
    replica_conn.reset();
    primary_conn = open_env(paths.primary);
    replica_conn = open_env(paths.replica);

    {
        mdbxc::sync::SyncEngine restarted_primary(primary_conn);
        mdbxc::sync::SyncEngine restarted_replica(replica_conn);
        initialize_engine(restarted_primary, primary_node, db_id);
        initialize_engine(restarted_replica, replica_node, db_id);
        const std::chrono::steady_clock::time_point restart_finish =
            std::chrono::steady_clock::now();

        const std::uint64_t restart_first_seq =
            scenario.historical_chunks_per_origin + scenario.new_chunks_per_origin + 1;
        const std::chrono::steady_clock::time_point restart_seed_start =
            std::chrono::steady_clock::now();
        const std::uint64_t restart_seeded =
            append_changelog_range(primary_conn, scenario, restart_first_seq,
                                   scenario.new_chunks_per_origin);
        const std::chrono::steady_clock::time_point restart_seed_finish =
            std::chrono::steady_clock::now();
        verify_origin_index(primary_conn, scenario, scenario.origins);

        PhaseMetrics restarted = run_sync_phase(
            "incremental_after_restart",
            scenario.new_chunks_per_origin,
            restart_seeded,
            elapsed_ms(restart_seed_start, restart_seed_finish),
            elapsed_ms(restart_start, restart_finish),
            primary_conn,
            replica_conn,
            restarted_primary,
            restarted_replica,
            scenario,
            replica_node,
            db_id);
        print_csv_row(scenario, restarted);
    }

    primary_conn->disconnect();
    replica_conn->disconnect();
}

int run(int argc, char** argv) {
    const std::vector<Scenario> scenarios = parse_scenarios(argc, argv);
    const std::string id = run_id();
    print_csv_header();
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        run_scenario(scenarios[i], id);
    }
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
