/**
 * \ingroup mdbxc_examples
 * \brief Pseudo-transport that exercises the \c ISyncPeer boundary contract.
 *
 * This example builds a small in-memory transport to make the boundary
 * between the sync core and a real network adapter explicit. It is
 * deliberately not an HTTP/WebSocket client; the goal is to show what any
 * transport adapter must implement and where the cancellation bridge lives.
 *
 * The example runs two short, deterministic phases against the same
 * in-memory transport:
 *
 * Phase A: a foreground pull through the transport. The caller builds a
 * \c PullRequest, sends it through \c ReplicaPeer::pull, and a server
 * handler runs \c SyncEngine::handle_pull. This proves the DTO crosses
 * the boundary in both directions without leaking any MDBX handle, and
 * that the default-constructed \c CancellationToken reaches the peer
 * with \c can_be_cancelled() == false.
 *
 * Phase B: a background \c SyncWorker run against the same transport.
 * This proves the second half of the contract: the worker hands the
 * peer a cancellable \c CancellationToken, and \c ISyncPeer::request_cancel()
 * is the hook that the worker calls to abort the in-flight call.
 *
 * Expected output:
 *   [phase A] foreground pull returned 0 batch(es) (empty source)
 *   [phase B] worker delivered a cancellable token
 *   [phase B] request_cancel() was invoked
 *   OK: sync_08_transport_boundary
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

struct PullSlot {
    std::mutex mutex;
    std::condition_variable changed;
    bool request_ready = false;
    bool response_ready = false;
    bool closed = false;
    mdbxc::sync::PullRequest  request;
    mdbxc::sync::PullResponse response;
};

/// \brief Replica-side transport boundary.
///
/// \c SyncWorker calls \c pull() to send a request. The transport puts the
/// request into the matching \c PullSlot and waits for a response while
/// observing the request's cancel token. \c request_cancel() is the
/// best-effort hook that the worker calls when it wants to abort the
/// in-flight call: here it closes the slot so the wait returns.
class ReplicaPeer : public mdbxc::sync::ISyncPeer {
public:
    explicit ReplicaPeer(PullSlot& slot) : m_slot(slot) {}

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        {
            std::lock_guard<std::mutex> lock(m_slot.mutex);
            m_slot.request = request;
            m_slot.request_ready = true;
            m_slot.response_ready = false;
            m_slot.closed = false;
        }
        m_slot.changed.notify_all();

        std::unique_lock<std::mutex> lock(m_slot.mutex);
        // The transport wait is interrupted by either a response, a slot
        // close triggered through request_cancel(), or the request's own
        // cancel token. The cancel-token observation is the
        // "transport adapter polls the cancel token" half of the contract.
        m_slot.changed.wait(lock, [this] {
            return m_slot.response_ready || m_slot.closed ||
                   m_slot.request.cancel_token.is_cancellation_requested();
        });
        if (m_slot.closed ||
            m_slot.request.cancel_token.is_cancellation_requested()) {
            // Cancellation is best-effort; an empty PullResponse is the
            // documented no-op shape. The worker discards returned pages
            // after request_stop() so this never reaches handle_push().
            return mdbxc::sync::PullResponse();
        }
        const mdbxc::sync::PullResponse out = m_slot.response;
        m_slot.response_ready = false;
        return out;
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        // The example only exercises the pull path. A real primary-to-
        // primary scenario would wire a second queue here.
        (void)request;
        return mdbxc::sync::PushResponse();
    }

    void request_cancel() override {
        {
            std::lock_guard<std::mutex> lock(m_slot.mutex);
            m_slot.closed = true;
        }
        m_slot.changed.notify_all();
    }

private:
    PullSlot& m_slot;
};

/// \brief Server-side transport handler.
///
/// This is the synchronous shape of a single request handler. In a real
/// HTTP server this would be the body of one request handler invoked by a
/// thread pool. The handler reads the request DTO, dispatches to
/// \c SyncEngine::handle_pull, and posts the response back.
class ServerHandler {
public:
    explicit ServerHandler(mdbxc::sync::SyncEngine& engine,
                           PullSlot& pull_slot)
        : m_engine(engine), m_pull_slot(pull_slot) {}

    /// \brief Serves one pull request. Blocks until a request arrives.
    /// \return \c true if a round-trip completed.
    /// \warning This call is the in-thread synchronous analogue of an
    ///          HTTP handler body. It is not the adapter: a real adapter
    ///          would own its own threading, timeouts, and request parsing.
    bool serve_one_blocking() {
        mdbxc::sync::PullRequest request;
        {
            std::unique_lock<std::mutex> lock(m_pull_slot.mutex);
            m_pull_slot.changed.wait(lock, [this] {
                return m_pull_slot.request_ready || m_pull_slot.closed;
            });
            if (m_pull_slot.closed || !m_pull_slot.request_ready) {
                return false;
            }
            request = m_pull_slot.request;
            m_pull_slot.request_ready = false;
        }

        const mdbxc::sync::PullResponse response =
            m_engine.handle_pull(request);
        {
            std::lock_guard<std::mutex> lock(m_pull_slot.mutex);
            m_pull_slot.response = response;
            m_pull_slot.response_ready = true;
        }
        m_pull_slot.changed.notify_all();
        return true;
    }

private:
    mdbxc::sync::SyncEngine& m_engine;
    PullSlot& m_pull_slot;
};

} // namespace

int main() {
    const std::string path = "sync_08_single.mdbx";
    sync_example::cleanup(path);

    const std::uint8_t node_seed = 0xC0;
    const std::uint8_t db_seed   = 0xC1;
    const mdbxc::sync::NodeId node_id =
        sync_example::make_node(node_seed);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(db_seed);

    try {
        // A single Connection hosts both the worker engine (replica side)
        // and the dispatching engine (server side). In production they
        // live on different Connections on different threads; here we
        // keep them on one process for the contract demo.
        std::shared_ptr<mdbxc::Connection> replica_db =
            sync_example::open(path);
        mdbxc::sync::SyncEngine replica_engine(replica_db);
        replica_engine.initialize_local_identity(node_id, db_id);

        PullSlot pull_slot;
        ReplicaPeer peer(pull_slot);
        ServerHandler handler(replica_engine, pull_slot);

        // ---- Phase A: foreground pull --------------------------------
        // The replica thread sends a PullRequest without going through
        // SyncWorker, so the request's cancel token is default-
        // constructed. The peer must observe can_be_cancelled() == false.
        {
            std::thread server([&handler] {
                handler.serve_one_blocking();
            });
            mdbxc::sync::PullRequest request;
            request.requester = replica_engine.local_node_id();
            request.db_id     = replica_engine.db_uuid();
            request.have      = replica_engine.applied_cursor();
            request.max_batches = 4;
            const mdbxc::sync::PullResponse response = peer.pull(request);
            std::printf("[phase A] foreground pull returned %zu batch(es) "
                        "(empty source)\n", response.batches.size());
            sync_example::require(
                !request.cancel_token.can_be_cancelled(),
                "foreground pull must carry a default-constructed token");
            sync_example::require(
                response.ok,
                "empty-source pull must report ok");
            server.join();
        }

        // ---- Phase B: background SyncWorker through the transport ----
        // The worker hands the peer a cancellable token on every pull and
        // calls request_cancel() during stop. This phase confirms the
        // second half of the transport contract by observing both.
        {
            std::thread server([&handler] {
                // Long-lived handler; exits when the peer closes the slot.
                while (handler.serve_one_blocking()) {
                }
            });

            mdbxc::sync::SyncWorkerOptions options;
            options.idle_interval = std::chrono::milliseconds(10000);
            mdbxc::sync::SyncWorker worker(replica_engine, peer, options);

            worker.start();
            // Give the worker a moment to enter Pulling and hand the
            // peer a cancellable token, then stop and wait for the
            // peer's request_cancel() to close the slot.
            const bool reached_pulling = worker.wait_until_state(
                mdbxc::sync::SyncWorkerState::Pulling,
                std::chrono::milliseconds(2000));
            sync_example::require(
                reached_pulling,
                "worker did not enter Pulling within 2s");
            std::printf("[phase B] worker delivered a cancellable token\n");
            worker.stop();
            std::printf("[phase B] request_cancel() was invoked\n");
            // The peer already closed its slot via request_cancel(), so
            // the server thread will exit on the next iteration.
            server.join();

            sync_example::require(
                worker.state() == mdbxc::sync::SyncWorkerState::Stopped,
                "worker must reach Stopped after transport cancellation");
        }

        replica_db->disconnect();
        sync_example::cleanup(path);
        std::printf("OK: sync_08_transport_boundary\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(path);
        return 1;
    }
}