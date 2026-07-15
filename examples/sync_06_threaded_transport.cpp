/**
 * \ingroup mdbxc_examples
 * \brief Advanced example: two thread-local programs and a pull wire.
 *
 * The two threads model two separate processes in one executable:
 *   - writer thread owns its DB, SyncEngine, capture sink, and write loop;
 *   - reader thread owns its DB, SyncEngine, read/sync loop, and replica;
 *   - PullRequest and PullResponse cross a shared in-memory "wire" buffer.
 *
 * The condition variable is only a compact stand-in for transport. A real
 * application can replace PullWire with HTTP, WebSocket, IPC, or a message
 * queue as long as only detached sync DTOs cross the boundary.
 *
 * Expected output:
 *   [writer] cycle 1 committed
 *   [wire] PullResponse: ...
 *   [reader] applied ...
 *   ...
 *   OK: sync_06_threaded_transport
 */

#include "sync_example_utils.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

const int writer_cycles = 10;
const int writer_ops_per_cycle = 3;
const std::uint64_t expected_writer_batches =
    static_cast<std::uint64_t>(writer_cycles * writer_ops_per_cycle);

struct PullWire {
    std::mutex mutex;
    std::condition_variable changed;
    // No MDBX transaction, cursor, table, Connection, or SyncEngine crosses
    // this boundary. Only protocol DTOs are copied between the threads.
    bool request_ready = false;
    bool response_ready = false;
    bool writer_done = false;
    bool stop_requested = false;
    mdbxc::sync::PullRequest request;
    mdbxc::sync::PullResponse response;
};

void print_reader_quotes(const std::shared_ptr<mdbxc::Connection>& reader_db) {
    mdbxc::KeyValueTable<int, std::string> quotes(reader_db, "quotes");
    auto txn = reader_db->transaction(mdbxc::TransactionMode::READ_ONLY);

    std::string btc;
    std::string eth;
    std::string sol;
    const bool has_btc = quotes.try_get(1, btc, txn.handle());
    const bool has_eth = quotes.try_get(2, eth, txn.handle());
    const bool has_sol = quotes.try_get(3, sol, txn.handle());

    std::printf("[reader replica] ");
    std::printf("BTC=%s ", has_btc ? btc.c_str() : "<deleted>");
    std::printf("ETH=%s ", has_eth ? eth.c_str() : "<missing>");
    std::printf("SOL=%s\n", has_sol ? sol.c_str() : "<missing>");
}

} // namespace

int main() {
    const std::string writer_path = "sync_06_writer.mdbx";
    const std::string reader_path = "sync_06_reader.mdbx";
    sync_example::cleanup(writer_path);
    sync_example::cleanup(reader_path);

    // Demo-only deterministic IDs. Production code should persist real 16-byte
    // node/database identifiers and reuse them across restarts.
    const std::uint8_t writer_node_seed = 0xE0;
    const std::uint8_t reader_node_seed = 0xE1;
    const std::uint8_t logical_db_seed = 0xE2;
    const mdbxc::sync::NodeId writer_node =
        sync_example::make_node(writer_node_seed);
    const mdbxc::sync::NodeId reader_node =
        sync_example::make_node(reader_node_seed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(logical_db_seed);

    PullWire wire;
    std::exception_ptr writer_error;
    std::exception_ptr reader_error;

    std::thread writer(
        [&wire, writer_path, writer_node, db_id, &writer_error] {
            std::shared_ptr<mdbxc::Connection> writer_db;
            bool capture_attached = false;
            try {
                writer_db = sync_example::open(writer_path);
                mdbxc::sync::SyncEngine writer_engine(writer_db);
                writer_engine.initialize_local_identity(writer_node, db_id);

                mdbxc::sync::ThreadLocalChangeAccumulator capture(writer_db);
                writer_db->attach_sync_capture(&capture);
                capture_attached = true;

                mdbxc::KeyValueTable<int, std::string> quotes(writer_db,
                                                              "quotes");

                auto serve_pull_requests_until =
                    [&wire, &writer_engine](
                        std::chrono::steady_clock::time_point deadline)
                        -> bool {
                        while (std::chrono::steady_clock::now() < deadline) {
                            mdbxc::sync::PullRequest request;
                            {
                                std::unique_lock<std::mutex> lock(wire.mutex);
                                const bool has_work = wire.changed.wait_until(
                                    lock,
                                    deadline,
                                    [&wire] {
                                        return wire.request_ready ||
                                               wire.stop_requested;
                                    });
                                if (!has_work) {
                                    return true;
                                }
                                if (wire.stop_requested) {
                                    return false;
                                }
                                request = wire.request;
                                wire.request_ready = false;
                            }
                            wire.changed.notify_all();

                            // This is the writer-side transport handler. A
                            // real server would deserialize PullRequest, call
                            // handle_pull(), then serialize PullResponse.
                            mdbxc::sync::PullResponse response =
                                writer_engine.handle_pull(request);

                            {
                                std::lock_guard<std::mutex> lock(wire.mutex);
                                wire.response = response;
                                wire.response_ready = true;
                            }
                            wire.changed.notify_all();
                        }
                        return true;
                    };

                for (int cycle = 0; cycle < writer_cycles; ++cycle) {
                    // Each loop commits three public table operations and
                    // therefore produces three local changelog batches.
                    quotes.insert_or_assign(
                        1,
                        "BTC " + std::to_string(65000 + cycle));
                    quotes.insert_or_assign(
                        2,
                        "ETH " + std::to_string(3500 + cycle));
                    if (cycle % 3 == 2) {
                        quotes.erase(3);
                    } else {
                        quotes.insert_or_assign(
                            3,
                            "SOL " + std::to_string(180 + cycle));
                    }

                    std::printf("[writer] cycle %d committed\n", cycle + 1);

                    const std::chrono::steady_clock::time_point deadline =
                        std::chrono::steady_clock::now() +
                        std::chrono::seconds(1);
                    if (!serve_pull_requests_until(deadline)) {
                        break;
                    }
                }

                if (capture_attached) {
                    writer_db->detach_sync_capture();
                    capture_attached = false;
                }

                {
                    std::lock_guard<std::mutex> lock(wire.mutex);
                    wire.writer_done = true;
                }
                wire.changed.notify_all();

                while (true) {
                    {
                        std::lock_guard<std::mutex> lock(wire.mutex);
                        if (wire.stop_requested) {
                            break;
                        }
                    }
                    const std::chrono::steady_clock::time_point deadline =
                        std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(100);
                    if (!serve_pull_requests_until(deadline)) {
                        break;
                    }
                }

                writer_db->disconnect();
            } catch (...) {
                writer_error = std::current_exception();
                if (capture_attached && writer_db) {
                    try {
                        writer_db->detach_sync_capture();
                    } catch (...) {
                    }
                }
                if (writer_db) {
                    try {
                        writer_db->disconnect();
                    } catch (...) {
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(wire.mutex);
                    wire.writer_done = true;
                    wire.stop_requested = true;
                }
                wire.changed.notify_all();
            }
        });

    std::thread reader(
        [&wire, reader_path, writer_node, reader_node, db_id, &reader_error] {
            std::shared_ptr<mdbxc::Connection> reader_db;
            try {
                reader_db = sync_example::open(reader_path);
                mdbxc::sync::SyncEngine reader_engine(reader_db);
                reader_engine.initialize_local_identity(reader_node, db_id);

                for (;;) {
                    mdbxc::sync::PullRequest pull;
                    pull.requester = reader_node;
                    pull.db_id = db_id;
                    pull.have = reader_engine.applied_cursor();
                    // Small enough to show repeated pull/apply cycles while
                    // the writer is still producing data.
                    pull.max_batches = 4;

                    mdbxc::sync::PullResponse pulled;
                    {
                        std::unique_lock<std::mutex> lock(wire.mutex);
                        wire.changed.wait(
                            lock,
                            [&wire] {
                                return (!wire.request_ready &&
                                        !wire.response_ready) ||
                                       wire.stop_requested;
                            });
                        if (wire.stop_requested) {
                            throw std::runtime_error(
                                "wire stopped before request was sent");
                        }
                        wire.request = pull;
                        wire.request_ready = true;
                    }
                    wire.changed.notify_all();

                    {
                        std::unique_lock<std::mutex> lock(wire.mutex);
                        wire.changed.wait(
                            lock,
                            [&wire] {
                                return wire.response_ready ||
                                       wire.stop_requested;
                            });
                        if (!wire.response_ready) {
                            throw std::runtime_error(
                                "wire stopped before response arrived");
                        }
                        pulled = wire.response;
                        wire.response_ready = false;
                    }
                    wire.changed.notify_all();

                    if (!pulled.batches.empty() || pulled.has_more) {
                        std::printf("[wire] PullResponse: %zu batch(es), "
                                    "has_more=%s\n",
                                    pulled.batches.size(),
                                    pulled.has_more ? "true" : "false");
                    }

                    if (!pulled.ok) {
                        throw std::runtime_error("pull failed: " +
                                                 pulled.error);
                    }
                    if (!pulled.batches.empty()) {
                        mdbxc::sync::PushRequest push;
                        push.sender = writer_node;
                        push.db_id = db_id;
                        push.batches = pulled.batches;

                        const mdbxc::sync::PushResponse applied =
                            reader_engine.handle_push(push);
                        if (!applied.ok) {
                            throw std::runtime_error("apply failed: " +
                                                     applied.error);
                        }

                        std::printf("[reader] applied %zu batch(es)\n",
                                    pulled.batches.size());
                        print_reader_quotes(reader_db);
                    }

                    const std::uint64_t replicated =
                        reader_engine.applied_cursor().last_seq_for(
                            writer_node);
                    bool writer_done = false;
                    {
                        std::lock_guard<std::mutex> lock(wire.mutex);
                        writer_done = wire.writer_done;
                    }
                    if (writer_done &&
                        replicated >= expected_writer_batches) {
                        break;
                    }
                    if (!pulled.has_more) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(150));
                    }
                }

                mdbxc::KeyValueTable<int, std::string> quotes(reader_db,
                                                              "quotes");
                sync_example::require(
                    sync_example::kv_or_throw(reader_db,
                                              quotes,
                                              1,
                                              "quotes[1]") == "BTC 65009",
                    "reader BTC row mismatch");
                sync_example::require(
                    sync_example::kv_or_throw(reader_db,
                                              quotes,
                                              2,
                                              "quotes[2]") == "ETH 3509",
                    "reader ETH row mismatch");
                sync_example::require(
                    sync_example::kv_or_throw(reader_db,
                                              quotes,
                                              3,
                                              "quotes[3]") == "SOL 189",
                    "reader SOL row mismatch");

                reader_db->disconnect();
            } catch (...) {
                reader_error = std::current_exception();
                if (reader_db) {
                    try {
                        reader_db->disconnect();
                    } catch (...) {
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(wire.mutex);
                    wire.stop_requested = true;
                }
                wire.changed.notify_all();
            }
        });

    reader.join();
    {
        std::lock_guard<std::mutex> lock(wire.mutex);
        wire.stop_requested = true;
    }
    wire.changed.notify_all();
    writer.join();

    int rc = 0;
    try {
        if (writer_error) {
            std::rethrow_exception(writer_error);
        }
        if (reader_error) {
            std::rethrow_exception(reader_error);
        }
        std::printf("OK: sync_06_threaded_transport\n");
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        rc = 1;
    }

    sync_example::cleanup(writer_path);
    sync_example::cleanup(reader_path);
    return rc;
}
