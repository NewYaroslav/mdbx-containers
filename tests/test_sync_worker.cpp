/// \file test_sync_worker.cpp
/// \brief Background SyncWorker lifecycle tests.

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-lck").c_str());
}

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId node{};
    for (int i = 0; i < 16; ++i) {
        node[i] = static_cast<std::uint8_t>(seed + i);
    }
    return node;
}

std::shared_ptr<mdbxc::Connection> open_env(const std::string& path) {
    mdbxc::Config config;
    config.pathname = path;
    config.max_dbs = 16;
    config.no_subdir = true;
    return mdbxc::Connection::create(config);
}

template<class KVT>
typename KVT::value_type::second_type kv_or_throw(
        const std::shared_ptr<mdbxc::Connection>& conn,
        KVT& kv,
        const typename KVT::value_type::first_type& key,
        const char* what) {
    typename KVT::value_type::second_type out{};
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    if (!kv.try_get(key, out, txn.handle())) {
        throw std::runtime_error(std::string("missing: ") + what);
    }
    return out;
}

class EmptyPeer : public mdbxc::sync::ISyncPeer {
public:
    EmptyPeer() : m_pull_count(0), m_cancel_count(0) {}

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        (void)request;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_pull_count;
        }
        m_changed.notify_all();
        return mdbxc::sync::PullResponse();
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        (void)request;
        return mdbxc::sync::PushResponse();
    }

    void request_cancel() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_cancel_count;
    }

    bool wait_for_pulls(int count, std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this, count] { return m_pull_count >= count; });
    }

    int cancel_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_count;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    int m_pull_count;
    int m_cancel_count;
};

class BlockingPeer : public mdbxc::sync::ISyncPeer {
public:
    explicit BlockingPeer(const mdbxc::sync::PullResponse& response)
        : m_response(response), m_entered(false), m_release(false),
          m_cancel_count(0) {}

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        (void)request;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_changed.notify_all();
        m_changed.wait(lock, [this] { return m_release; });
        return m_response;
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        (void)request;
        return mdbxc::sync::PushResponse();
    }

    void request_cancel() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_cancel_count;
    }

    bool wait_until_entered(std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this] { return m_entered; });
    }

    int cancel_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_count;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_release = true;
        }
        m_changed.notify_all();
    }

private:
    mdbxc::sync::PullResponse m_response;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    bool m_entered;
    bool m_release;
    int  m_cancel_count;
};

class CancelableBlockingPeer : public mdbxc::sync::ISyncPeer {
public:
    explicit CancelableBlockingPeer(const mdbxc::sync::PullResponse& response)
        : m_response(response), m_entered(false), m_cancelled(false),
          m_cancel_count(0) {}

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        (void)request;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_changed.notify_all();
        m_changed.wait(lock, [this] { return m_cancelled; });
        return m_response;
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        (void)request;
        return mdbxc::sync::PushResponse();
    }

    void request_cancel() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cancelled = true;
            ++m_cancel_count;
        }
        m_changed.notify_all();
    }

    bool wait_until_entered(std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this] { return m_entered; });
    }

    int cancel_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_count;
    }

private:
    mdbxc::sync::PullResponse m_response;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    bool m_entered;
    bool m_cancelled;
    int  m_cancel_count;
};

class TokenBlockingPeer : public mdbxc::sync::ISyncPeer {
public:
    explicit TokenBlockingPeer(const mdbxc::sync::PullResponse& response)
        : m_response(response), m_entered(false), m_saw_token(false),
          m_saw_token_cancel(false) {}

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_saw_token = request.cancel_token.can_be_cancelled();
        m_changed.notify_all();
        while (!request.cancel_token.is_cancellation_requested()) {
            m_changed.wait_for(lock, std::chrono::milliseconds(10));
        }
        m_saw_token_cancel = true;
        return m_response;
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        (void)request;
        return mdbxc::sync::PushResponse();
    }

    bool wait_until_entered(std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this] { return m_entered; });
    }

    bool saw_token() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_saw_token;
    }

    bool saw_token_cancel() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_saw_token_cancel;
    }

private:
    mdbxc::sync::PullResponse m_response;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    bool m_entered;
    bool m_saw_token;
    bool m_saw_token_cancel;
};

class FailingPeer : public mdbxc::sync::ISyncPeer {
public:
    FailingPeer() : m_cancel_count(0) {}

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        (void)request;
        mdbxc::sync::PullResponse response;
        response.ok = false;
        response.error = "synthetic pull failure";
        return response;
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        (void)request;
        return mdbxc::sync::PushResponse();
    }

    void request_cancel() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_cancel_count;
    }

    int cancel_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_count;
    }

private:
    mutable std::mutex m_mutex;
    int m_cancel_count;
};

class SelfStoppingPeer : public mdbxc::sync::ISyncPeer {
public:
    SelfStoppingPeer() : m_worker(nullptr), m_saw_logic_error(false) {}

    void set_worker(mdbxc::sync::SyncWorker* worker) {
        m_worker = worker;
    }

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        (void)request;
        if (m_worker == nullptr) {
            throw std::runtime_error("self-stopping peer has no worker");
        }
        try {
            m_worker->stop();
        } catch (const std::logic_error& e) {
            if (std::string(e.what()).find("cannot join itself") ==
                std::string::npos) {
                throw;
            }
            m_saw_logic_error = true;
        }
        return mdbxc::sync::PullResponse();
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        (void)request;
        return mdbxc::sync::PushResponse();
    }

    bool saw_logic_error() const {
        return m_saw_logic_error;
    }

private:
    mdbxc::sync::SyncWorker* m_worker;
    bool m_saw_logic_error;
};

class RecordingWorkerObserver : public mdbxc::sync::ISyncWorkerObserver {
public:
    struct BackoffRecord {
        mdbxc::sync::SyncWorkerRoundResult result;
        std::chrono::milliseconds delay;
    };

    void on_sync_worker_page_applied(
            const mdbxc::sync::SyncWorkerPageEvent& event) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pages.push_back(event);
        }
        m_changed.notify_all();
    }

    void on_sync_worker_round_completed(
            const mdbxc::sync::SyncWorkerRoundResult& result) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_rounds.push_back(result);
        }
        m_changed.notify_all();
    }

    void on_sync_worker_backoff(
            const mdbxc::sync::SyncWorkerRoundResult& result,
            std::chrono::milliseconds delay) override {
        BackoffRecord record;
        record.result = result;
        record.delay = delay;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_backoffs.push_back(record);
        }
        m_changed.notify_all();
    }

    std::vector<mdbxc::sync::SyncWorkerPageEvent> pages() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pages;
    }

    std::vector<mdbxc::sync::SyncWorkerRoundResult> rounds() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_rounds;
    }

    std::vector<BackoffRecord> backoffs() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_backoffs;
    }

    bool wait_for_backoffs(std::size_t count,
                           std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this, count] { return m_backoffs.size() >= count; });
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::vector<mdbxc::sync::SyncWorkerPageEvent> m_pages;
    std::vector<mdbxc::sync::SyncWorkerRoundResult> m_rounds;
    std::vector<BackoffRecord> m_backoffs;
};

class ThrowingRoundObserver : public mdbxc::sync::ISyncWorkerObserver {
public:
    void on_sync_worker_round_completed(
            const mdbxc::sync::SyncWorkerRoundResult& result) override {
        (void)result;
        throw std::runtime_error("observer boom");
    }
};

void expect_invalid_options(mdbxc::sync::SyncEngine& engine,
                            mdbxc::sync::ISyncPeer& peer,
                            const mdbxc::sync::SyncWorkerOptions& options,
                            const char* name) {
    bool threw = false;
    try {
        mdbxc::sync::SyncWorker worker(engine, peer, options);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!threw) {
        throw std::runtime_error(
            std::string("SyncWorker accepted invalid options: ") + name);
    }
}

void test_worker_run_once_drains_paginated_pull() {
    using namespace mdbxc;
    const std::string primary_path = "test_worker_primary.mdbx";
    const std::string replica_path = "test_worker_replica.mdbx";
    cleanup(primary_path);
    cleanup(replica_path);

    std::shared_ptr<Connection> primary_conn = open_env(primary_path);
    std::shared_ptr<Connection> replica_conn = open_env(replica_path);

    const sync::NodeId primary_node = make_node(0xA0);
    const sync::NodeId replica_node = make_node(0xB0);
    const sync::NodeId db_id = make_node(0xD0);

    sync::SyncEngine primary_engine(primary_conn);
    sync::SyncEngine replica_engine(replica_conn);
    primary_engine.initialize_local_identity(primary_node, db_id);
    replica_engine.initialize_local_identity(replica_node, db_id);

    sync::ThreadLocalChangeAccumulator sink(primary_conn);
    primary_conn->attach_sync_capture(&sink);
    {
        KeyValueTable<int, int> kv(primary_conn, "kv");
        for (int i = 1; i <= 5; ++i) {
            kv.insert_or_assign(i, i * 10);
        }
    }
    primary_conn->detach_sync_capture();

    sync::DirectSyncPeer peer(&primary_engine);
    RecordingWorkerObserver observer;
    sync::SyncWorkerOptions options;
    options.max_batches = 2;
    options.idle_interval = std::chrono::milliseconds(1);
    options.observer = &observer;

    sync::SyncWorker worker(replica_engine, peer, options);
    const sync::SyncWorkerRoundResult result = worker.run_once();
    if (!result.ok) {
        throw std::runtime_error("worker run_once failed: " + result.error);
    }
    if (result.pages_pulled != 3u || result.batches_applied != 5u) {
        throw std::runtime_error("worker did not drain paginated pull");
    }
    if (worker.state() != sync::SyncWorkerState::Stopped) {
        throw std::runtime_error("worker run_once should finish Stopped");
    }
    const std::vector<sync::SyncWorkerPageEvent> pages = observer.pages();
    if (pages.size() != 3u) {
        throw std::runtime_error("worker observer did not see all pages");
    }
    if (pages[0].batches_applied != 2u || !pages[0].has_more ||
        pages[0].applied_cursor.last_seq_for(primary_node) != 2u) {
        throw std::runtime_error("worker observer first page mismatch");
    }
    if (pages[1].batches_applied != 2u || !pages[1].has_more ||
        pages[1].applied_cursor.last_seq_for(primary_node) != 4u) {
        throw std::runtime_error("worker observer second page mismatch");
    }
    if (pages[2].batches_applied != 1u || pages[2].has_more ||
        pages[2].applied_cursor.last_seq_for(primary_node) != 5u) {
        throw std::runtime_error("worker observer final page mismatch");
    }
    const std::vector<sync::SyncWorkerRoundResult> rounds = observer.rounds();
    if (rounds.size() != 1u || !rounds[0].ok ||
        rounds[0].pages_pulled != 3u ||
        rounds[0].batches_applied != 5u) {
        throw std::runtime_error("worker observer round mismatch");
    }

    KeyValueTable<int, int> replica_kv(replica_conn, "kv");
    for (int i = 1; i <= 5; ++i) {
        if (kv_or_throw(replica_conn, replica_kv, i, "replica kv") != i * 10) {
            throw std::runtime_error("replica value mismatch after worker sync");
        }
    }

    primary_conn->disconnect();
    replica_conn->disconnect();
    cleanup(primary_path);
    cleanup(replica_path);
}

void test_worker_start_stop_idle() {
    using namespace mdbxc;
    const std::string path = "test_worker_idle.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    EmptyPeer peer;
    sync::SyncWorkerOptions options;
    options.idle_interval = std::chrono::milliseconds(10000);
    sync::SyncWorker worker(engine, peer, options);

    worker.start();
    if (!peer.wait_for_pulls(1, std::chrono::milliseconds(2000))) {
        throw std::runtime_error("worker did not perform initial pull");
    }
    if (!worker.wait_until_state(sync::SyncWorkerState::Idle,
                                 std::chrono::milliseconds(2000))) {
        throw std::runtime_error("worker did not enter Idle");
    }
    worker.stop();
    if (worker.state() != sync::SyncWorkerState::Stopped) {
        throw std::runtime_error("worker did not stop");
    }
    if (peer.cancel_count() != 0) {
        throw std::runtime_error("idle stop should not cancel peer transport");
    }

    conn->disconnect();
    cleanup(path);
}

void test_worker_backoff_on_pull_error() {
    using namespace mdbxc;
    const std::string path = "test_worker_backoff.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    FailingPeer peer;
    RecordingWorkerObserver observer;
    sync::SyncWorkerOptions options;
    options.initial_backoff = std::chrono::milliseconds(10000);
    options.max_backoff = std::chrono::milliseconds(10000);
    options.observer = &observer;
    sync::SyncWorker worker(engine, peer, options);

    worker.start();
    if (!worker.wait_until_state(sync::SyncWorkerState::Backoff,
                                 std::chrono::milliseconds(2000))) {
        throw std::runtime_error("worker did not enter Backoff after pull error");
    }
    if (worker.last_error().find("synthetic pull failure") == std::string::npos) {
        throw std::runtime_error("worker did not record pull error");
    }
    if (!observer.wait_for_backoffs(1u, std::chrono::milliseconds(2000))) {
        throw std::runtime_error("worker observer did not see backoff");
    }
    const std::vector<sync::SyncWorkerRoundResult> rounds = observer.rounds();
    if (rounds.empty() || rounds[0].ok ||
        rounds[0].error.find("synthetic pull failure") == std::string::npos) {
        throw std::runtime_error("worker observer round error mismatch");
    }
    const std::vector<RecordingWorkerObserver::BackoffRecord> backoffs =
        observer.backoffs();
    if (backoffs[0].delay != std::chrono::milliseconds(10000) ||
        backoffs[0].result.ok ||
        backoffs[0].result.error.find("synthetic pull failure") ==
            std::string::npos) {
        throw std::runtime_error("worker observer backoff mismatch");
    }
    worker.stop();
    if (worker.state() != sync::SyncWorkerState::Stopped) {
        throw std::runtime_error("backoff worker did not stop");
    }
    if (peer.cancel_count() != 0) {
        throw std::runtime_error("backoff stop should not cancel peer transport");
    }

    conn->disconnect();
    cleanup(path);
}

void test_worker_observer_exception_does_not_fail_round() {
    using namespace mdbxc;
    const std::string path = "test_worker_observer_exception.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    EmptyPeer peer;
    ThrowingRoundObserver observer;
    sync::SyncWorkerOptions options;
    options.observer = &observer;
    sync::SyncWorker worker(engine, peer, options);

    const sync::SyncWorkerRoundResult result = worker.run_once();
    if (!result.ok) {
        throw std::runtime_error("observer exception failed sync round");
    }
    if (worker.last_error().find("observer boom") == std::string::npos) {
        throw std::runtime_error("observer exception was not recorded");
    }

    conn->disconnect();
    cleanup(path);
}

void test_worker_rejects_invalid_options() {
    using namespace mdbxc;
    const std::string path = "test_worker_invalid_options.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    EmptyPeer peer;
    sync::SyncWorkerOptions options;

    sync::SyncWorkerOptions invalid = options;
    invalid.max_batches = 0;
    expect_invalid_options(engine, peer, invalid, "max_batches");

    invalid = options;
    invalid.max_bytes = 0;
    expect_invalid_options(engine, peer, invalid, "max_bytes");

    invalid = options;
    invalid.idle_interval = std::chrono::milliseconds(-1);
    expect_invalid_options(engine, peer, invalid, "idle_interval");

    invalid = options;
    invalid.initial_backoff = std::chrono::milliseconds(-1);
    expect_invalid_options(engine, peer, invalid, "initial_backoff");

    invalid = options;
    invalid.max_backoff = std::chrono::milliseconds(-1);
    expect_invalid_options(engine, peer, invalid, "max_backoff");

    invalid = options;
    invalid.initial_backoff = std::chrono::milliseconds(10);
    invalid.max_backoff = std::chrono::milliseconds(9);
    expect_invalid_options(engine, peer, invalid, "max_backoff ordering");

    conn->disconnect();
    cleanup(path);
}

void test_worker_stop_while_pull_blocked_does_not_apply_returned_page() {
    using namespace mdbxc;
    const std::string path = "test_worker_blocked_stop.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    const sync::NodeId replica_node = make_node(0xB0);
    const sync::NodeId remote_origin = make_node(0xA0);
    const sync::NodeId db_id = make_node(0xD0);

    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(replica_node, db_id);

    sync::ChangeBatch batch;
    batch.origin_node_id = remote_origin;
    batch.seq = 1;

    sync::PullResponse response;
    response.batches.push_back(batch);

    BlockingPeer peer(response);
    sync::SyncWorkerOptions options;
    options.idle_interval = std::chrono::milliseconds(10000);
    sync::SyncWorker worker(engine, peer, options);

    worker.start();
    if (!peer.wait_until_entered(std::chrono::milliseconds(2000))) {
        throw std::runtime_error("worker did not enter blocking pull");
    }
    worker.request_stop();
    if (peer.cancel_count() != 1) {
        throw std::runtime_error("blocked worker did not request cancellation");
    }
    peer.release();
    worker.join();

    if (worker.state() != sync::SyncWorkerState::Stopped) {
        throw std::runtime_error("blocked worker did not stop");
    }
    if (engine.applied_cursor().last_seq_for(remote_origin) != 0u) {
        throw std::runtime_error("worker applied a page returned after stop");
    }

    conn->disconnect();
    cleanup(path);
}

void test_worker_repeated_stop_cancels_blocking_peer_once() {
    using namespace mdbxc;
    const std::string path = "test_worker_cancel_once.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    const sync::NodeId replica_node = make_node(0xB1);
    const sync::NodeId remote_origin = make_node(0xA1);
    const sync::NodeId db_id = make_node(0xD1);

    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(replica_node, db_id);

    sync::ChangeBatch batch;
    batch.origin_node_id = remote_origin;
    batch.seq = 1;

    sync::PullResponse response;
    response.batches.push_back(batch);

    BlockingPeer peer(response);
    sync::SyncWorkerOptions options;
    options.idle_interval = std::chrono::milliseconds(10000);
    sync::SyncWorker worker(engine, peer, options);

    worker.start();
    if (!peer.wait_until_entered(std::chrono::milliseconds(2000))) {
        throw std::runtime_error("worker did not enter blocking pull");
    }
    worker.request_stop();
    worker.request_stop();

    if (peer.cancel_count() != 1) {
        throw std::runtime_error("repeated stop cancelled active peer more than once");
    }

    peer.release();
    worker.join();

    if (worker.state() != sync::SyncWorkerState::Stopped) {
        throw std::runtime_error("cancel-once worker did not stop");
    }
    if (engine.applied_cursor().last_seq_for(remote_origin) != 0u) {
        throw std::runtime_error("worker applied a page after repeated stop");
    }

    conn->disconnect();
    cleanup(path);
}

void test_worker_stop_cancels_blocking_peer() {
    using namespace mdbxc;
    const std::string path = "test_worker_cancel_peer.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    const sync::NodeId replica_node = make_node(0xB0);
    const sync::NodeId remote_origin = make_node(0xA0);
    const sync::NodeId db_id = make_node(0xD0);

    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(replica_node, db_id);

    sync::ChangeBatch batch;
    batch.origin_node_id = remote_origin;
    batch.seq = 1;

    sync::PullResponse response;
    response.batches.push_back(batch);

    CancelableBlockingPeer peer(response);
    sync::SyncWorkerOptions options;
    options.idle_interval = std::chrono::milliseconds(10000);
    sync::SyncWorker worker(engine, peer, options);

    worker.start();
    if (!peer.wait_until_entered(std::chrono::milliseconds(2000))) {
        throw std::runtime_error("worker did not enter cancellable pull");
    }
    worker.request_stop();
    worker.join();

    if (peer.cancel_count() == 0) {
        throw std::runtime_error("worker did not request peer cancellation");
    }
    if (worker.state() != sync::SyncWorkerState::Stopped) {
        throw std::runtime_error("cancelled worker did not stop");
    }
    if (engine.applied_cursor().last_seq_for(remote_origin) != 0u) {
        throw std::runtime_error("worker applied a page returned after cancel");
    }

    conn->disconnect();
    cleanup(path);
}

void test_worker_stop_cancels_request_token() {
    using namespace mdbxc;
    const std::string path = "test_worker_cancel_token.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    const sync::NodeId replica_node = make_node(0xB2);
    const sync::NodeId remote_origin = make_node(0xA2);
    const sync::NodeId db_id = make_node(0xD2);

    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(replica_node, db_id);

    sync::ChangeBatch batch;
    batch.origin_node_id = remote_origin;
    batch.seq = 1;

    sync::PullResponse response;
    response.batches.push_back(batch);

    TokenBlockingPeer peer(response);
    sync::SyncWorkerOptions options;
    options.idle_interval = std::chrono::milliseconds(10000);
    sync::SyncWorker worker(engine, peer, options);

    worker.start();
    if (!peer.wait_until_entered(std::chrono::milliseconds(2000))) {
        throw std::runtime_error("worker did not enter token-blocking pull");
    }
    if (!peer.saw_token()) {
        throw std::runtime_error("worker did not pass a cancellable token");
    }
    worker.request_stop();
    worker.join();

    if (!peer.saw_token_cancel()) {
        throw std::runtime_error("worker did not cancel request token");
    }
    if (worker.state() != sync::SyncWorkerState::Stopped) {
        throw std::runtime_error("token-cancelled worker did not stop");
    }
    if (engine.applied_cursor().last_seq_for(remote_origin) != 0u) {
        throw std::runtime_error("worker applied a page returned after token cancel");
    }

    conn->disconnect();
    cleanup(path);
}

void test_worker_rejects_self_join() {
    using namespace mdbxc;
    const std::string path = "test_worker_self_join.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xB0), make_node(0xD0));

    SelfStoppingPeer peer;
    sync::SyncWorker worker(engine, peer);
    peer.set_worker(&worker);

    worker.start();
    worker.join();

    if (!peer.saw_logic_error()) {
        throw std::runtime_error("worker did not reject self-join");
    }
    if (worker.state() != sync::SyncWorkerState::Stopped) {
        throw std::runtime_error("self-join worker did not stop");
    }

    conn->disconnect();
    cleanup(path);
}

} // namespace

int main() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "test_worker_run_once_drains_paginated_pull",
          &test_worker_run_once_drains_paginated_pull },
        { "test_worker_start_stop_idle", &test_worker_start_stop_idle },
        { "test_worker_backoff_on_pull_error", &test_worker_backoff_on_pull_error },
        { "test_worker_observer_exception_does_not_fail_round",
          &test_worker_observer_exception_does_not_fail_round },
        { "test_worker_rejects_invalid_options",
          &test_worker_rejects_invalid_options },
        { "test_worker_stop_while_pull_blocked",
          &test_worker_stop_while_pull_blocked_does_not_apply_returned_page },
        { "test_worker_repeated_stop_cancels_blocking_peer_once",
          &test_worker_repeated_stop_cancels_blocking_peer_once },
        { "test_worker_stop_cancels_blocking_peer",
          &test_worker_stop_cancels_blocking_peer },
        { "test_worker_stop_cancels_request_token",
          &test_worker_stop_cancels_request_token },
        { "test_worker_rejects_self_join", &test_worker_rejects_self_join },
    };

    int rc = 0;
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        try {
            cases[i].fn();
            std::printf("PASS %s\n", cases[i].name);
        } catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", cases[i].name, e.what());
            rc = static_cast<int>(i + 1);
        } catch (...) {
            std::printf("FAIL %s: non-std exception\n", cases[i].name);
            rc = static_cast<int>(i + 1);
        }
    }
    return rc;
}
