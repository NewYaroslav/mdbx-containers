/**
 * \ingroup mdbxc_examples
 * \brief Multi-process HTTP sync node fleet demo.
 *
 * This example starts several copies of itself through tiny-process-library.
 * Every child process owns its own MDBX environment, HTTP server, sync engine,
 * capture sink, and application loop. The only thing crossing process
 * boundaries is HTTP sync DTO traffic.
 *
 * Default run:
 *
 *   ./sync_18_http_node_fleet
 *
 * It executes two short scenarios:
 *   - master-replica: node A writes, node B only pulls from A;
 *   - mesh: nodes A and B both write and pull from each other.
 *
 * Manual node mode:
 *
 *   ./sync_18_http_node_fleet node A 127.0.0.1 18220 a.mdbx \
 *       16 208 127.0.0.1 18221 32 1 1
 *
 * Type "stop" on stdin to shut the node down.
 */

#include <mdbx_containers/sync/transports/simple_web/HttpTransport.hpp>
#include <mdbx_containers/tables.hpp>

#include "sync_example_utils.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <process.hpp>

namespace {

const std::uint8_t kDbSeed = 0xD0;
const std::uint16_t kMasterBasePort = 18220;
const std::uint16_t kMeshBasePort = 18230;

int parse_int(const char* text, const char* label) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        throw std::invalid_argument(std::string("invalid ") + label);
    }
    return static_cast<int>(value);
}

std::uint8_t parse_byte_seed(const char* text, const char* label) {
    const int value = parse_int(text, label);
    if (value < 0 || value > 255) {
        throw std::invalid_argument(std::string("invalid ") + label);
    }
    return static_cast<std::uint8_t>(value);
}

std::uint16_t parse_port(const char* text, const char* label) {
    const int value = parse_int(text, label);
    if (value <= 0 || value > 65535) {
        throw std::invalid_argument(std::string("invalid ") + label);
    }
    return static_cast<std::uint16_t>(value);
}

struct NodeOptions {
    std::string name;
    std::string host;
    std::uint16_t port;
    std::string db_path;
    std::uint8_t node_seed;
    std::uint8_t db_seed;
    std::string peer_host;
    std::uint16_t peer_port;
    std::uint8_t peer_node_seed;
    bool active_writer;
    bool fresh_db;
};

void write_activity(const NodeOptions& options,
                    const std::shared_ptr<mdbxc::Connection>& conn,
                    mdbxc::KeyValueTable<int, std::string>& events,
                    mdbxc::KeyTable<int>& live_ids,
                    mdbxc::ValueTable<std::string>& status,
                    int tick) {
    mdbxc::Transaction txn =
        conn->transaction(mdbxc::TransactionMode::WRITABLE);
    const int key = static_cast<int>(options.node_seed) * 1000 + tick;
    const int old_key = key - 3;

    std::ostringstream text;
    text << options.name << " tick " << tick;
    events.insert_or_assign(key, text.str(), txn);
    live_ids.insert(key, txn);

    std::ostringstream state;
    state << options.name << " local tick " << tick;
    status.set(state.str(), txn);

    if (tick % 4 == 0) {
        events.erase(old_key, txn);
        live_ids.erase(old_key, txn);
    }

    txn.commit();
    std::printf("[%s] local write tick=%d key=%d\n",
                options.name.c_str(),
                tick,
                key);
}

std::size_t pull_once(const NodeOptions& options,
                      mdbxc::sync::SyncEngine& local_engine,
                      mdbxc::sync::HttpSyncPeer& peer,
                      std::mutex& db_mutex) {
    mdbxc::sync::PullRequest pull;
    pull.requester = sync_example::make_node(options.node_seed);
    pull.db_id = sync_example::make_node(options.db_seed);
    pull.max_batches = 8;
    pull.max_bytes = 1024 * 1024;
    {
        std::lock_guard<std::mutex> lock(db_mutex);
        pull.have = local_engine.applied_cursor();
    }

    const mdbxc::sync::PullResponse response = peer.pull(pull);
    if (!response.ok) {
        throw std::runtime_error("pull failed: " + response.error);
    }
    if (response.batches.empty()) {
        return 0;
    }

    mdbxc::sync::PushRequest push;
    push.sender = sync_example::make_node(options.peer_node_seed);
    push.db_id = pull.db_id;
    push.batches = response.batches;

    mdbxc::sync::PushResponse applied;
    {
        std::lock_guard<std::mutex> lock(db_mutex);
        applied = local_engine.handle_push(push);
    }
    if (!applied.ok) {
        throw std::runtime_error("apply failed: " + applied.error);
    }
    return response.batches.size();
}

void print_snapshot(const NodeOptions& options,
                    mdbxc::KeyValueTable<int, std::string>& events,
                    mdbxc::KeyTable<int>& live_ids,
                    mdbxc::ValueTable<std::string>& status,
                    std::mutex& db_mutex) {
    std::lock_guard<std::mutex> lock(db_mutex);
    const std::map<int, std::string> event_rows =
        events.retrieve_all<std::map>();
    const std::set<int> live_rows = live_ids.retrieve_all<std::set>();
    std::string status_text;
    const bool has_status = status.try_get(status_text);

    std::printf("[%s] snapshot events=%u live_ids=%u status=%s\n",
                options.name.c_str(),
                static_cast<unsigned>(event_rows.size()),
                static_cast<unsigned>(live_rows.size()),
                has_status ? status_text.c_str() : "<empty>");
}

int run_node(const NodeOptions& options) {
    if (options.fresh_db) {
        sync_example::cleanup(options.db_path);
    }

    std::shared_ptr<mdbxc::Connection> conn =
        sync_example::open(options.db_path);
    std::mutex db_mutex;

    mdbxc::sync::SyncEngine engine(conn);
    engine.initialize_local_identity(
        sync_example::make_node(options.node_seed),
        sync_example::make_node(options.db_seed));

    mdbxc::sync::ThreadLocalChangeAccumulator capture(conn);
    conn->attach_sync_capture(&capture);

    mdbxc::KeyValueTable<int, std::string> events(conn, "fleet_events");
    mdbxc::KeyTable<int> live_ids(conn, "fleet_live_ids");
    mdbxc::ValueTable<std::string> status(conn, "fleet_status");

    mdbxc::sync::HttpSyncServer http_handler(engine);
    mdbxc::sync::simple_web::HttpSyncListenerConfig listener_config;
    listener_config.host = options.host;
    listener_config.port = options.port;
    listener_config.handler_mutex = &db_mutex;
    mdbxc::sync::simple_web::HttpSyncListener http_server(
        http_handler, listener_config);
    http_server.start();
    std::printf("[%s] listening on %s:%u\n",
                options.name.c_str(),
                options.host.c_str(),
                static_cast<unsigned>(http_server.port()));

    mdbxc::sync::simple_web::HttpSyncClientConfig client_config;
    client_config.host = options.peer_host;
    client_config.port = options.peer_port;
    client_config.timeout = std::chrono::seconds(2);
    mdbxc::sync::simple_web::HttpSyncClient http_client(client_config);
    mdbxc::sync::HttpSyncPeer peer(http_client);

    std::atomic<bool> pause_writes(false);
    std::atomic<bool> stop_requested(false);
    std::thread stdin_thread(
        [&pause_writes, &stop_requested, &options]() {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line == "pause") {
                    pause_writes.store(true, std::memory_order_release);
                    std::printf("[%s] pause command received\n",
                                options.name.c_str());
                    continue;
                }
                if (line == "stop" || line == "quit" || line == "exit") {
                    stop_requested.store(true, std::memory_order_release);
                    std::printf("[%s] stop command received\n",
                                options.name.c_str());
                    return;
                }
            }
            stop_requested.store(true, std::memory_order_release);
        });

    int tick = 0;
    while (!stop_requested.load(std::memory_order_acquire)) {
        ++tick;
        try {
            if (options.active_writer &&
                !pause_writes.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(db_mutex);
                write_activity(options, conn, events, live_ids, status, tick);
            }

            const std::size_t pulled =
                pull_once(options, engine, peer, db_mutex);
            if (pulled != 0) {
                std::printf("[%s] pulled %u batch(es) from peer\n",
                            options.name.c_str(),
                            static_cast<unsigned>(pulled));
            }
        } catch (const std::exception& e) {
            std::printf("[%s] loop warning: %s\n",
                        options.name.c_str(),
                        e.what());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }

    if (stdin_thread.joinable()) {
        stdin_thread.join();
    }
    print_snapshot(options, events, live_ids, status, db_mutex);

    http_server.stop();
    conn->detach_sync_capture();
    sync_example::disconnect_and_cleanup(conn, options.db_path);
    std::printf("[%s] stopped\n", options.name.c_str());
    return 0;
}

NodeOptions parse_node_options(int argc, char** argv) {
    if (argc != 13) {
        throw std::invalid_argument(
            "node mode expects: node <name> <host> <port> <db_path> "
            "<node_seed> <db_seed> <peer_host> <peer_port> "
            "<peer_node_seed> <active_writer> <fresh_db>");
    }

    NodeOptions options;
    options.name = argv[2];
    options.host = argv[3];
    options.port = parse_port(argv[4], "port");
    options.db_path = argv[5];
    options.node_seed = parse_byte_seed(argv[6], "node_seed");
    options.db_seed = parse_byte_seed(argv[7], "db_seed");
    options.peer_host = argv[8];
    options.peer_port = parse_port(argv[9], "peer_port");
    options.peer_node_seed = parse_byte_seed(argv[10], "peer_node_seed");
    options.active_writer = parse_int(argv[11], "active_writer") != 0;
    options.fresh_db = parse_int(argv[12], "fresh_db") != 0;
    return options;
}

std::vector<std::string> node_command(const std::string& executable,
                                      const std::string& name,
                                      std::uint16_t port,
                                      const std::string& db_path,
                                      std::uint8_t node_seed,
                                      std::uint16_t peer_port,
                                      std::uint8_t peer_seed,
                                      bool active_writer) {
    std::vector<std::string> args;
    args.push_back(executable);
    args.push_back("node");
    args.push_back(name);
    args.push_back("127.0.0.1");
    args.push_back(std::to_string(static_cast<unsigned>(port)));
    args.push_back(db_path);
    args.push_back(std::to_string(static_cast<unsigned>(node_seed)));
    args.push_back(std::to_string(static_cast<unsigned>(kDbSeed)));
    args.push_back("127.0.0.1");
    args.push_back(std::to_string(static_cast<unsigned>(peer_port)));
    args.push_back(std::to_string(static_cast<unsigned>(peer_seed)));
    args.push_back(active_writer ? "1" : "0");
    args.push_back("1");
    return args;
}

class ChildNode {
public:
    ChildNode(const std::string& name,
              const std::vector<std::string>& command)
        : m_name(name),
          m_process(
              command,
              std::string(),
              [this](const char* bytes, std::size_t n) {
                  print_child_output(bytes, n, false);
              },
              [this](const char* bytes, std::size_t n) {
                  print_child_output(bytes, n, true);
              },
              true) {}

    void stop() {
        m_process.write("stop\n");
        m_process.close_stdin();
    }

    void pause() {
        m_process.write("pause\n");
    }

    int wait() {
        return m_process.get_exit_status();
    }

private:
    void print_child_output(const char* bytes,
                            std::size_t n,
                            bool is_error) {
        std::lock_guard<std::mutex> lock(output_mutex());
        std::printf("[%s %s] %s",
                    m_name.c_str(),
                    is_error ? "stderr" : "stdout",
                    std::string(bytes, n).c_str());
    }

    static std::mutex& output_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    std::string m_name;
    TinyProcessLib::Process m_process;
};

int run_supervisor_scenario(const std::string& executable,
                            const std::string& scenario,
                            unsigned seconds) {
    const bool mesh = scenario == "mesh";
    const std::uint16_t base_port =
        mesh ? kMeshBasePort : kMasterBasePort;
    const std::string prefix =
        mesh ? "sync_18_mesh_" : "sync_18_master_";

    sync_example::cleanup(prefix + "A.mdbx");
    sync_example::cleanup(prefix + "B.mdbx");

    std::printf("[supervisor] starting %s scenario for %u second(s)\n",
                scenario.c_str(),
                seconds);

    ChildNode node_a(
        "A",
        node_command(executable,
                     "A",
                     base_port,
                     prefix + "A.mdbx",
                     0x10,
                     static_cast<std::uint16_t>(base_port + 1),
                     0x20,
                     true));
    ChildNode node_b(
        "B",
        node_command(executable,
                     "B",
                     static_cast<std::uint16_t>(base_port + 1),
                     prefix + "B.mdbx",
                     0x20,
                     base_port,
                     0x10,
                     mesh));

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    node_a.pause();
    node_b.pause();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    node_a.stop();
    node_b.stop();

    const int a_status = node_a.wait();
    const int b_status = node_b.wait();
    std::printf("[supervisor] %s exits: A=%d B=%d\n",
                scenario.c_str(),
                a_status,
                b_status);
    return a_status == 0 && b_status == 0 ? 0 : 1;
}

void print_usage(const char* executable) {
    std::printf("Usage:\n");
    std::printf("  %s\n", executable);
    std::printf("  %s master-replica [seconds]\n", executable);
    std::printf("  %s mesh [seconds]\n", executable);
    std::printf("  %s node <name> <host> <port> <db_path> "
                "<node_seed> <db_seed> <peer_host> <peer_port> "
                "<peer_node_seed> <active_writer> <fresh_db>\n",
                executable);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "node") {
            return run_node(parse_node_options(argc, argv));
        }

        const unsigned seconds =
            argc >= 3
                ? static_cast<unsigned>(parse_int(argv[2], "seconds"))
                : 5u;

        if (argc == 1) {
            const int one_way =
                run_supervisor_scenario(argv[0], "master-replica", seconds);
            const int mesh =
                run_supervisor_scenario(argv[0], "mesh", seconds);
            if (one_way == 0 && mesh == 0) {
                std::printf("OK: sync_18_http_node_fleet\n");
                return 0;
            }
            return 1;
        }

        const std::string scenario = argv[1];
        if (scenario == "master-replica" || scenario == "mesh") {
            const int rc = run_supervisor_scenario(
                argv[0], scenario, seconds);
            if (rc == 0) {
                std::printf("OK: sync_18_http_node_fleet (%s)\n",
                            scenario.c_str());
            }
            return rc;
        }

        print_usage(argv[0]);
        return 2;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        print_usage(argv[0]);
        return 1;
    }
}
