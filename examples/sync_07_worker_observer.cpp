/**
 * \ingroup mdbxc_examples
 * \brief SyncWorker observer for background replica updates.
 *
 * This example keeps the transport simple with DirectSyncPeer and focuses on
 * the application-facing part of SyncWorker:
 *   - the primary commits local table changes;
 *   - the replica runs SyncWorker in the background;
 *   - ISyncWorkerObserver reports applied pages and completed rounds;
 *   - the foreground code wakes up and reads the updated replica table.
 *
 * Expected output:
 *   [writer] cycle 1 committed
 *   [worker observer] page ...
 *   [replica] BTC=... ETH=... SOL=...
 *   ...
 *   OK: sync_07_worker_observer
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

const int writer_cycles = 3;
const int writes_per_cycle = 3;

const char* stage_name(mdbxc::sync::SyncWorkerStage stage) {
    switch (stage) {
        case mdbxc::sync::SyncWorkerStage::RoundStarted:
            return "round-started";
        case mdbxc::sync::SyncWorkerStage::PullStarted:
            return "pull-started";
        case mdbxc::sync::SyncWorkerStage::PullFinished:
            return "pull-finished";
        case mdbxc::sync::SyncWorkerStage::ApplyStarted:
            return "apply-started";
        case mdbxc::sync::SyncWorkerStage::ApplyFinished:
            return "apply-finished";
        case mdbxc::sync::SyncWorkerStage::RoundCompleted:
            return "round-completed";
        case mdbxc::sync::SyncWorkerStage::BackoffStarted:
            return "backoff-started";
    }
    return "unknown";
}

class ConsoleWorkerObserver : public mdbxc::sync::ISyncWorkerObserver {
public:
    explicit ConsoleWorkerObserver(const mdbxc::sync::NodeId& origin)
        : m_origin(origin), m_pages(0), m_batches(0), m_rounds(0),
          m_backoffs(0) {}

    void on_sync_worker_stage_changed(
            const mdbxc::sync::SyncWorkerStageEvent& event) override {
        if (event.stage != mdbxc::sync::SyncWorkerStage::PullStarted &&
            event.stage != mdbxc::sync::SyncWorkerStage::ApplyFinished) {
            return;
        }
        const std::size_t page =
            event.stage == mdbxc::sync::SyncWorkerStage::PullStarted
                ? event.pages_pulled + 1
                : event.pages_pulled;
        std::lock_guard<std::mutex> lock(m_mutex);
        std::printf(
            "[worker observer] stage=%s page=%zu page_batches=%zu "
            "total_batches=%zu has_more=%s\n",
            stage_name(event.stage),
            page,
            event.batches_in_page,
            event.batches_applied,
            event.has_more ? "true" : "false");
    }

    void on_sync_worker_page_applied(
            const mdbxc::sync::SyncWorkerPageEvent& event) override {
        const unsigned long long origin_seq =
            static_cast<unsigned long long>(
                event.applied_cursor.last_seq_for(m_origin));
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_pages;
            m_batches += event.batches_applied;
            std::printf(
                "[worker observer] page %zu applied %zu batch(es), "
                "origin_seq=%llu, has_more=%s\n",
                m_pages,
                event.batches_applied,
                origin_seq,
                event.has_more ? "true" : "false");
        }
        m_changed.notify_all();
    }

    void on_sync_worker_round_completed(
            const mdbxc::sync::SyncWorkerRoundResult& result) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_rounds;
            std::printf(
                "[worker observer] round %zu ok=%s pages=%zu batches=%zu\n",
                m_rounds,
                result.ok ? "true" : "false",
                result.pages_pulled,
                result.batches_applied);
        }
        m_changed.notify_all();
    }

    void on_sync_worker_backoff(
            const mdbxc::sync::SyncWorkerRoundResult& result,
            std::chrono::milliseconds delay) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_backoffs;
            std::printf(
                "[worker observer] backoff %zu for %lld ms: %s\n",
                m_backoffs,
                static_cast<long long>(delay.count()),
                result.error.c_str());
        }
        m_changed.notify_all();
    }

    bool wait_for_batches(std::size_t count,
                          std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock,
            timeout,
            [this, count] { return m_batches >= count; });
    }

private:
    mdbxc::sync::NodeId m_origin;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::size_t m_pages;
    std::size_t m_batches;
    std::size_t m_rounds;
    std::size_t m_backoffs;
};

void print_replica_quotes(const std::shared_ptr<mdbxc::Connection>& replica) {
    mdbxc::KeyValueTable<int, std::string> quotes(replica, "quotes");
    auto txn = replica->transaction(mdbxc::TransactionMode::READ_ONLY);

    std::string btc;
    std::string eth;
    std::string sol;
    const bool has_btc = quotes.try_get(1, btc, txn.handle());
    const bool has_eth = quotes.try_get(2, eth, txn.handle());
    const bool has_sol = quotes.try_get(3, sol, txn.handle());

    std::printf("[replica] ");
    std::printf("BTC=%s ", has_btc ? btc.c_str() : "<missing>");
    std::printf("ETH=%s ", has_eth ? eth.c_str() : "<missing>");
    std::printf("SOL=%s\n", has_sol ? sol.c_str() : "<deleted>");
}

} // namespace

int main() {
    const std::string primary_path = "sync_07_primary.mdbx";
    const std::string replica_path = "sync_07_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const std::uint8_t primary_node_seed = 0xF0;
    const std::uint8_t replica_node_seed = 0xF1;
    const std::uint8_t logical_db_seed = 0xF2;
    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(primary_node_seed);
    const mdbxc::sync::NodeId replica_node =
        sync_example::make_node(replica_node_seed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(logical_db_seed);

    try {
        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica =
            sync_example::open(replica_path);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_engine(replica);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        mdbxc::sync::ThreadLocalChangeAccumulator capture(primary);
        mdbxc::KeyValueTable<int, std::string> primary_quotes(primary,
                                                              "quotes");

        mdbxc::sync::DirectSyncPeer primary_peer(&primary_engine);
        ConsoleWorkerObserver observer(primary_node);

        mdbxc::sync::SyncWorkerOptions options;
        options.max_batches = 2;
        options.idle_interval = std::chrono::milliseconds(20);
        options.initial_backoff = std::chrono::milliseconds(50);
        options.max_backoff = std::chrono::milliseconds(200);
        options.observer = &observer;

        mdbxc::sync::SyncWorker worker(replica_engine, primary_peer, options);
        bool worker_started = false;

        std::size_t expected_batches = 0;
        for (int cycle = 0; cycle < writer_cycles; ++cycle) {
            primary->attach_sync_capture(&capture);
            primary_quotes.insert_or_assign(
                1,
                "BTC " + std::to_string(65000 + cycle));
            primary_quotes.insert_or_assign(
                2,
                "ETH " + std::to_string(3500 + cycle));
            if (cycle == 1) {
                primary_quotes.erase(3);
            } else {
                primary_quotes.insert_or_assign(
                    3,
                    "SOL " + std::to_string(180 + cycle));
            }
            primary->detach_sync_capture();

            expected_batches += writes_per_cycle;
            std::printf("[writer] cycle %d committed\n", cycle + 1);
            if (!worker_started) {
                // Start after the first local batch exists on the primary.
                // That keeps the example focused on progress notifications
                // instead of an initial empty-pull/backoff path.
                worker.start();
                worker_started = true;
            }
            sync_example::require(
                observer.wait_for_batches(expected_batches,
                                          std::chrono::seconds(5)),
                "timed out waiting for replica observer");
            print_replica_quotes(replica);
        }

        worker.stop();

        mdbxc::KeyValueTable<int, std::string> replica_quotes(replica,
                                                              "quotes");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_quotes, 1,
                                      "replica BTC") == "BTC 65002",
            "replica BTC mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_quotes, 2,
                                      "replica ETH") == "ETH 3502",
            "replica ETH mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_quotes, 3,
                                      "replica SOL") == "SOL 182",
            "replica SOL mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_07_worker_observer\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
