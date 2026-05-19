#include <mdbx_containers/KeyValueTable.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

mdbxc::Config make_config(const std::string& name) {
    mdbxc::Config cfg;
    cfg.pathname = "data/" + name + ".mdbx";
    cfg.max_dbs = 4;
    cfg.no_subdir = true;
    cfg.relative_to_exe = true;
    return cfg;
}

void strict_disconnect_example() {
    auto conn = mdbxc::Connection::create(make_config("shutdown_disconnect"));

    {
        mdbxc::KeyValueTable<int, std::string> table(conn, "items");
        table.insert_or_assign(1, "closed after all work is done");
    }

    // Use disconnect() only when no transactions/cursors are alive.
    conn->disconnect();
    std::cout << "disconnect(): closed a clean lifecycle\n";
}

void coordinated_shutdown_example() {
    auto conn = mdbxc::Connection::create(make_config("shutdown_blocking"));
    std::atomic<bool> stop_requested(false);

    std::thread worker(
        [conn, &stop_requested]() {
            mdbxc::KeyValueTable<int, std::string> table(conn, "events");
            int counter = 0;

            while (!stop_requested.load()) {
                auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
                table.insert_or_assign(counter++, "worker tick", txn);
                txn.commit();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop_requested.store(true);

    // shutdown() rejects new transactions, waits for open transaction handles,
    // and then disconnects.
    conn->shutdown();
    worker.join();
    std::cout << "shutdown(): waited and closed\n";
}

void bounded_shutdown_example() {
    auto conn = mdbxc::Connection::create(make_config("shutdown_timeout"));

    std::mutex sync_mutex;
    std::condition_variable sync_cv;
    bool txn_open = false;
    bool finish_worker = false;

    std::thread worker(
        [conn, &sync_mutex, &sync_cv, &txn_open, &finish_worker]() {
            conn->begin(mdbxc::TransactionMode::WRITABLE);

            {
                std::lock_guard<std::mutex> lock(sync_mutex);
                txn_open = true;
            }
            sync_cv.notify_one();

            {
                std::unique_lock<std::mutex> lock(sync_mutex);
                sync_cv.wait(lock, [&finish_worker]() {
                    return finish_worker;
                });
            }

            conn->rollback();
        });

    {
        std::unique_lock<std::mutex> lock(sync_mutex);
        sync_cv.wait(lock, [&txn_open]() {
            return txn_open;
        });
    }

    // shutdown_for() is useful when a service stop must remain bounded.
    if (!conn->shutdown_for(std::chrono::milliseconds(50))) {
        std::cout << "shutdown_for(): timed out, asking worker to finish\n";
    }

    {
        std::lock_guard<std::mutex> lock(sync_mutex);
        finish_worker = true;
    }
    sync_cv.notify_one();
    worker.join();

    // The shutdown request remains active after timeout, so retrying is safe.
    conn->shutdown();
    std::cout << "shutdown_for(): retried and closed\n";
}

} // namespace

int main() {
    strict_disconnect_example();
    coordinated_shutdown_example();
    bounded_shutdown_example();

    return 0;
}
