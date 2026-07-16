/// \file test_transport_boundary.cpp
/// \brief Contract tests for \c ISyncPeer adapters at the transport boundary.
///
/// These tests pin down what any \c ISyncPeer implementation must observe in
/// order to interoperate with \c SyncEngine and \c SyncWorker. They are not
/// tests of \c SyncWorker itself (those live in \c test_sync_worker.cpp).
/// The contract under verification is the one documented in
/// \c include/mdbx_containers/sync/DESIGN.md under "Transport boundary
/// contract":
///
///   1. A transport adapter receives a cancellable \c CancellationToken on
///      every \c pull() when the caller is the \c SyncWorker. Manual
///      foreground callers that build \c PullRequest themselves and do not
///      attach a source hand a default (non-cancellable) token.
///   2. \c ISyncPeer::request_cancel() is best-effort: the default no-op
///      implementation is valid and must satisfy the type. Adapters that
///      can interrupt blocking calls override it.
///   3. Only sync DTOs cross the boundary; \c ISyncPeer implementations
///      must not capture or mutate the caller's \c Connection / \c
///      SyncEngine across threads.

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

/// Peer that records what it observed on the request side of \c pull().
/// Used to assert the contract that the \c SyncWorker delivers a
/// cancellable \c CancellationToken to adapter implementations.
class RecordingPeer : public mdbxc::sync::ISyncPeer {
public:
    RecordingPeer() = default;

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_pull_count;
            m_last_token_can_be_cancelled = request.cancel_token.can_be_cancelled();
        }
        m_changed.notify_all();
        return mdbxc::sync::PullResponse();
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_push_count;
            m_last_push_token_can_be_cancelled =
                request.cancel_token.can_be_cancelled();
        }
        m_changed.notify_all();
        return mdbxc::sync::PushResponse();
    }

    bool wait_for_pulls(std::size_t count,
                        std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this, count] { return m_pull_count >= count; });
    }

    bool wait_for_pushes(std::size_t count,
                         std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout,
            [this, count] { return m_push_count >= count; });
    }

    std::size_t pull_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pull_count;
    }

    std::size_t push_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_push_count;
    }

    bool last_pull_token_can_be_cancelled() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_token_can_be_cancelled;
    }

    bool last_push_token_can_be_cancelled() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_push_token_can_be_cancelled;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::size_t m_pull_count = 0;
    std::size_t m_push_count = 0;
    bool m_last_token_can_be_cancelled = false;
    bool m_last_push_token_can_be_cancelled = false;
};

/// Minimal \c ISyncPeer that uses the default \c request_cancel() no-op.
/// Documents that the default no-op is a valid adapter.
class DefaultNoOpPeer : public mdbxc::sync::ISyncPeer {
public:
    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        (void)request;
        return mdbxc::sync::PullResponse();
    }
    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        (void)request;
        return mdbxc::sync::PushResponse();
    }
    // No request_cancel() override: the default in ISyncPeer is a no-op.
    // This class exists to assert that the no-op is a valid contract.
};

void test_contract_sync_worker_delivers_cancellable_pull_token() {
    using namespace mdbxc;
    const std::string path = "test_transport_contract_worker_token.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xA0), make_node(0xD0));

    RecordingPeer peer;
    sync::SyncWorkerOptions options;
    options.idle_interval = std::chrono::milliseconds(10000);
    sync::SyncWorker worker(engine, peer, options);

    worker.start();
    if (!peer.wait_for_pulls(1u, std::chrono::milliseconds(2000))) {
        throw std::runtime_error(
            "worker never invoked ISyncPeer::pull()");
    }
    worker.stop();

    if (!peer.last_pull_token_can_be_cancelled()) {
        throw std::runtime_error(
            "transport contract violated: SyncWorker did not deliver a "
            "cancellable CancellationToken to ISyncPeer::pull()");
    }

    conn->disconnect();
    cleanup(path);
}

void test_contract_foreground_pull_token_is_default_constructed() {
    using namespace mdbxc;
    const std::string path = "test_transport_contract_foreground.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xA1), make_node(0xD1));

    // Foreground callers (anyone who builds PullRequest manually, including
    // DirectSyncPeer in examples) hand a default-constructed token to the
    // peer. Adapters that observe can_be_cancelled() == false must still
    // complete the call normally and not treat it as a cancellation signal.
    RecordingPeer peer;

    sync::PullRequest request;
    request.requester = engine.local_node_id();
    request.db_id = engine.db_uuid();
    request.have = engine.applied_cursor();
    request.max_batches = 10;
    // No CancellationSource attached: token stays default-constructed.

    const mdbxc::sync::PullResponse response = peer.pull(request);
    (void)response;

    if (peer.last_pull_token_can_be_cancelled()) {
        throw std::runtime_error(
            "transport contract violated: default-constructed "
            "CancellationToken must report can_be_cancelled() == false");
    }

    conn->disconnect();
    cleanup(path);
}

void test_contract_request_cancel_default_is_noop() {
    DefaultNoOpPeer peer;
    // Must compile and return cleanly. The default ISyncPeer::request_cancel()
    // is a no-op by design; adapters that only rely on CancellationToken
    // remain valid.
    peer.request_cancel();
    peer.request_cancel();
    peer.request_cancel();
}

void test_contract_dto_values_round_trip_through_peer() {
    // Confirms that PullRequest / PullResponse / PushRequest / PushResponse
    // can be copied between producer and consumer via ISyncPeer without
    // any MDBX handle or Connection reference leaking across the boundary.
    using namespace mdbxc;
    sync::PullRequest pull_request{};
    pull_request.requester = make_node(0x11);
    pull_request.db_id     = make_node(0x12);
    pull_request.have.last_seq_by_origin[make_node(0x99)] = 5u;
    pull_request.max_batches = 7u;
    pull_request.max_bytes = 1024u;
    pull_request.request_full_snapshot = true;

    sync::PullResponse pull_response{};
    pull_response.ok = true;
    pull_response.has_more = true;
    pull_response.remote_have.last_seq_by_origin[make_node(0x99)] = 5u;
    sync::ChangeBatch batch{};
    batch.origin_node_id = make_node(0xAA);
    batch.seq = 1u;
    pull_response.batches.push_back(batch);

    sync::PushRequest push_request{};
    push_request.sender = make_node(0x21);
    push_request.db_id  = make_node(0x22);
    push_request.batches = pull_response.batches;

    sync::PushResponse push_response{};
    push_response.ok = true;
    push_response.receiver_have.last_seq_by_origin[make_node(0xAA)] = 1u;

    struct CarrierPeer : sync::ISyncPeer {
        sync::PullRequest seen_pull_request{};
        sync::PullResponse sent_pull_response{};
        sync::PushRequest seen_push_request{};
        sync::PushResponse sent_push_response{};

        sync::PullResponse pull(
                const sync::PullRequest& request) override {
            seen_pull_request = request;
            return sent_pull_response;
        }
        sync::PushResponse push(
                const sync::PushRequest& request) override {
            seen_push_request = request;
            return sent_push_response;
        }
    };

    CarrierPeer carrier;
    carrier.sent_pull_response = pull_response;
    carrier.sent_push_response = push_response;
    const sync::PullResponse got_pull = carrier.pull(pull_request);
    const sync::PushResponse got_push = carrier.push(push_request);

    if (carrier.seen_pull_request.requester != pull_request.requester ||
        carrier.seen_pull_request.db_id != pull_request.db_id ||
        carrier.seen_pull_request.max_batches != pull_request.max_batches ||
        carrier.seen_pull_request.max_bytes != pull_request.max_bytes ||
        carrier.seen_pull_request.request_full_snapshot !=
            pull_request.request_full_snapshot ||
        carrier.seen_pull_request.have.last_seq_by_origin.size() != 1u) {
        throw std::runtime_error("PullRequest DTO did not round-trip via peer");
    }
    if (!got_pull.ok || !got_pull.has_more ||
        got_pull.remote_have.last_seq_for(make_node(0x99)) != 5u ||
        got_pull.batches.size() != 1u ||
        got_pull.batches[0].origin_node_id != make_node(0xAA) ||
        got_pull.batches[0].seq != 1u) {
        throw std::runtime_error("PullResponse shape was not honoured");
    }
    if (carrier.seen_push_request.sender != push_request.sender ||
        carrier.seen_push_request.db_id != push_request.db_id ||
        carrier.seen_push_request.batches.size() !=
            push_request.batches.size()) {
        throw std::runtime_error("PushRequest DTO did not round-trip via peer");
    }
    if (!got_push.ok ||
        got_push.receiver_have.last_seq_for(make_node(0xAA)) != 1u) {
        throw std::runtime_error("PushResponse shape was not honoured");
    }
}

void test_contract_threaded_pull_does_not_capture_connection() {
    // Confirms that an ISyncPeer implementation can run on a different
    // thread than the engine that produced the request, as long as the
    // adapter only touches the DTOs. The adapter itself is responsible
    // for any cross-thread synchronisation on its own state; the contract
    // requires the boundary to carry no MDBX handles or Connection.
    using namespace mdbxc;
    const std::string path = "test_transport_contract_threaded.mdbx";
    cleanup(path);

    std::shared_ptr<Connection> conn = open_env(path);
    sync::SyncEngine engine(conn);
    engine.initialize_local_identity(make_node(0xA3), make_node(0xD3));

    std::atomic<int> pull_calls{0};
    std::atomic<bool> saw_cancellable_token{false};

    struct ThreadedPeer : sync::ISyncPeer {
        std::atomic<int>* pull_calls;
        std::atomic<bool>* saw_cancellable_token;
        ThreadedPeer(std::atomic<int>* pc, std::atomic<bool>* saw)
            : pull_calls(pc), saw_cancellable_token(saw) {}
        sync::PullResponse pull(
                const sync::PullRequest& request) override {
            if (request.cancel_token.can_be_cancelled()) {
                saw_cancellable_token->store(true);
            }
            ++(*pull_calls);
            return sync::PullResponse();
        }
        sync::PushResponse push(
                const sync::PushRequest&) override {
            return sync::PushResponse();
        }
    } peer(&pull_calls, &saw_cancellable_token);

    sync::SyncWorkerOptions options;
    options.idle_interval = std::chrono::milliseconds(10000);
    sync::SyncWorker worker(engine, peer, options);
    worker.start();

    for (int i = 0; i < 200; ++i) {
        if (pull_calls.load() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    worker.stop();
    if (pull_calls.load() == 0) {
        throw std::runtime_error("worker thread did not call ISyncPeer::pull()");
    }
    if (!saw_cancellable_token.load()) {
        throw std::runtime_error(
            "worker thread did not deliver a cancellable CancellationToken");
    }

    conn->disconnect();
    cleanup(path);
}

} // namespace

int main() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "test_contract_sync_worker_delivers_cancellable_pull_token",
          &test_contract_sync_worker_delivers_cancellable_pull_token },
        { "test_contract_foreground_pull_token_is_default_constructed",
          &test_contract_foreground_pull_token_is_default_constructed },
        { "test_contract_request_cancel_default_is_noop",
          &test_contract_request_cancel_default_is_noop },
        { "test_contract_dto_values_round_trip_through_peer",
          &test_contract_dto_values_round_trip_through_peer },
        { "test_contract_threaded_pull_does_not_capture_connection",
          &test_contract_threaded_pull_does_not_capture_connection },
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
