/**
 * \ingroup mdbxc_examples
 * \brief SyncNodeSession, capture, and remote apply observer hooks.
 *
 * This example keeps DirectSyncPeer so the application-facing lifecycle is
 * visible without socket setup:
 *   - primary writes are captured for the session lifetime;
 *   - replica SyncWorker runs in the background through SyncNodeSession;
 *   - the replica connection reports committed remote apply through
 *     ISyncApplyObserver.
 *
 * Expected output:
 *   [apply observer] generation=1 batches=2 ops=2 dbis=1 item2_visible=yes
 *   [replica] item 1=alpha item 2=beta
 *   OK: sync_21_worker_guard_apply_hooks
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

namespace {

class ConsoleApplyObserver : public mdbxc::sync::ISyncApplyObserver {
public:
    ConsoleApplyObserver(
            std::shared_ptr<mdbxc::Connection> replica_conn,
            mdbxc::KeyValueTable<int, std::string>* replica_items)
        : m_events(0), m_last_generation(0), m_last_batches(0),
          m_last_ops(0), m_last_dbi_count(0), m_replica_conn(replica_conn),
          m_replica_items(replica_items), m_item_two_visible(false) {}

    void on_sync_apply_committed(
            const mdbxc::sync::SyncApplyEvent& event) override {
        bool item_two_visible = false;
        try {
            const std::string two = sync_example::kv_or_throw(
                m_replica_conn, *m_replica_items, 2,
                "observer replica item 2");
            item_two_visible = two == "beta";
        } catch (...) {
            item_two_visible = false;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_events;
            m_last_generation = event.generation;
            m_last_batches = event.applied_batches;
            m_last_ops = event.applied_ops;
            m_last_dbi_count = event.affected_dbi_names.size();
            m_item_two_visible = item_two_visible;
            std::printf(
                "[apply observer] generation=%llu batches=%zu ops=%zu "
                "dbis=%zu item2_visible=%s\n",
                static_cast<unsigned long long>(event.generation),
                event.applied_batches,
                event.applied_ops,
                event.affected_dbi_names.size(),
                item_two_visible ? "yes" : "no");
        }
        m_changed.notify_all();
    }

    bool wait_for_events(std::size_t count,
                         std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout, [this, count] { return m_events >= count; });
    }

    std::size_t events() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_events;
    }

    std::uint64_t last_generation() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_generation;
    }

    bool item_two_visible() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_item_two_visible;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::size_t m_events;
    std::uint64_t m_last_generation;
    std::size_t m_last_batches;
    std::size_t m_last_ops;
    std::size_t m_last_dbi_count;
    std::shared_ptr<mdbxc::Connection> m_replica_conn;
    mdbxc::KeyValueTable<int, std::string>* m_replica_items;
    bool m_item_two_visible;
};

} // namespace

int main() {
    const std::string primary_path = "sync_21_primary.mdbx";
    const std::string replica_path = "sync_21_replica.mdbx";

    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    std::shared_ptr<mdbxc::Connection> primary_conn;
    std::shared_ptr<mdbxc::Connection> replica_conn;

    try {
        const mdbxc::sync::NodeId primary_node =
            sync_example::make_node(0x21);
        const mdbxc::sync::NodeId replica_node =
            sync_example::make_node(0x22);
        const mdbxc::sync::NodeId db_id =
            sync_example::make_node(0xD1);

        primary_conn = sync_example::open(primary_path);
        replica_conn = sync_example::open(replica_path);

        mdbxc::sync::SyncEngine primary_engine(primary_conn);
        mdbxc::sync::SyncEngine replica_engine(replica_conn);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        mdbxc::sync::ThreadLocalChangeAccumulator capture(primary_conn);
        mdbxc::sync::DirectSyncPeer primary_peer(&primary_engine);
        mdbxc::KeyValueTable<int, std::string> primary_items(
            primary_conn, "items");
        mdbxc::KeyValueTable<int, std::string> replica_items(
            replica_conn, "items");

        mdbxc::sync::SyncWorkerOptions options;
        options.idle_interval = std::chrono::milliseconds(20);
        options.max_batches = 4;
        mdbxc::sync::SyncWorker worker(replica_engine, primary_peer, options);

        ConsoleApplyObserver apply_observer(replica_conn, &replica_items);
        mdbxc::sync::SyncNodeSessionOptions session_options;
        session_options.capture_connection = primary_conn;
        session_options.capture_sink = &capture;
        session_options.apply_observer_connection = replica_conn;
        session_options.apply_observer = &apply_observer;

        {
            mdbxc::sync::SyncNodeSession session(worker, session_options);

            primary_items.insert_or_assign(1, "alpha");
            primary_items.insert_or_assign(2, "beta");

            sync_example::require(
                apply_observer.wait_for_events(
                    1, std::chrono::milliseconds(2000)),
                "replica did not observe remote apply");

            const std::string one = sync_example::kv_or_throw(
                replica_conn, replica_items, 1, "replica item 1");
            const std::string two = sync_example::kv_or_throw(
                replica_conn, replica_items, 2, "replica item 2");
            sync_example::require(one == "alpha", "replica item 1 mismatch");
            sync_example::require(two == "beta", "replica item 2 mismatch");
            sync_example::require(
                apply_observer.last_generation() ==
                    replica_conn->sync_apply_generation(),
                "observer generation mismatch");
            sync_example::require(
                apply_observer.item_two_visible(),
                "observer did not see replica item through table API");

            std::printf("[replica] item 1=%s item 2=%s\n",
                        one.c_str(), two.c_str());
        }

        sync_example::disconnect_and_cleanup(primary_conn, primary_path);
        sync_example::disconnect_and_cleanup(replica_conn, replica_path);
        std::puts("OK: sync_21_worker_guard_apply_hooks");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "sync_21_worker_guard_apply_hooks failed: %s\n",
                     e.what());
    }

    sync_example::disconnect_and_cleanup(primary_conn, primary_path);
    sync_example::disconnect_and_cleanup(replica_conn, replica_path);
    return 1;
}
