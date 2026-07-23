/**
 * \ingroup mdbxc_examples
 * \brief Minimal application-facing SyncNodeSession wiring.
 *
 * The example uses DirectSyncPeer so it builds without optional HTTP
 * dependencies. In a socket-backed application, keep the same
 * SyncNodeSessionOptions shape and replace DirectSyncPeer with a ready-made
 * HTTP peer such as the Simple-Web or Kurlyk transport binding.
 *
 * Expected output:
 *   [primary] wrote two independent local transactions
 *   [replica] order 1=created order 2=paid
 *   OK: sync_22_node_session_minimal
 */

#include "sync_example_utils.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

bool wait_until_replicated(
        const std::shared_ptr<mdbxc::Connection>& replica_conn,
        mdbxc::KeyValueTable<int, std::string>& replica_orders,
        std::chrono::milliseconds timeout) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (sync_example::kv_has(replica_conn, replica_orders, 1) &&
            sync_example::kv_has(replica_conn, replica_orders, 2)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

int main() {
    const std::string primary_path = "sync_22_primary.mdbx";
    const std::string replica_path = "sync_22_replica.mdbx";

    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    std::shared_ptr<mdbxc::Connection> primary_conn;
    std::shared_ptr<mdbxc::Connection> replica_conn;

    try {
        const mdbxc::sync::NodeId primary_node =
            sync_example::make_node(0x31);
        const mdbxc::sync::NodeId replica_node =
            sync_example::make_node(0x32);
        const mdbxc::sync::NodeId db_id =
            sync_example::make_node(0xD2);

        primary_conn = sync_example::open(primary_path);
        replica_conn = sync_example::open(replica_path);

        mdbxc::sync::SyncEngine primary_engine(primary_conn);
        mdbxc::sync::SyncEngine replica_engine(replica_conn);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        mdbxc::sync::ThreadLocalChangeAccumulator capture(primary_conn);
        mdbxc::sync::DirectSyncPeer primary_peer(&primary_engine);

        mdbxc::sync::SyncWorkerOptions worker_options;
        worker_options.idle_interval = std::chrono::milliseconds(25);
        worker_options.max_batches = 8;
        mdbxc::sync::SyncWorker replica_worker(
            replica_engine, primary_peer, worker_options);

        mdbxc::sync::SyncNodeSessionOptions session_options;
        session_options.capture_connection = primary_conn;
        session_options.capture_sink = &capture;

        mdbxc::KeyValueTable<int, std::string> primary_orders(
            primary_conn, "orders");
        mdbxc::KeyValueTable<int, std::string> replica_orders(
            replica_conn, "orders");

        {
            mdbxc::sync::SyncNodeSession session(
                replica_worker, session_options);

            primary_orders.insert_or_assign(1, "created");
            primary_orders.insert_or_assign(2, "paid");
            std::puts("[primary] wrote two independent local transactions");

            sync_example::require(
                wait_until_replicated(replica_conn, replica_orders,
                                      std::chrono::milliseconds(2000)),
                "replica did not catch up");

            const std::string first = sync_example::kv_or_throw(
                replica_conn, replica_orders, 1, "replica order 1");
            const std::string second = sync_example::kv_or_throw(
                replica_conn, replica_orders, 2, "replica order 2");
            sync_example::require(first == "created",
                                  "replica order 1 mismatch");
            sync_example::require(second == "paid",
                                  "replica order 2 mismatch");

            std::printf("[replica] order 1=%s order 2=%s\n",
                        first.c_str(), second.c_str());
        }

        sync_example::disconnect_and_cleanup(primary_conn, primary_path);
        sync_example::disconnect_and_cleanup(replica_conn, replica_path);
        std::puts("OK: sync_22_node_session_minimal");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "sync_22_node_session_minimal failed: %s\n",
                     e.what());
    }

    sync_example::disconnect_and_cleanup(primary_conn, primary_path);
    sync_example::disconnect_and_cleanup(replica_conn, replica_path);
    return 1;
}
