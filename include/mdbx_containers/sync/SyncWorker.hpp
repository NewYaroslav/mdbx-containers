#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_SYNC_WORKER_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_SYNC_WORKER_HPP_INCLUDED

/// \file SyncWorker.hpp
/// \brief Background pull/apply loop for \c SyncEngine.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "ISyncPeer.hpp"
#include "SyncEngine.hpp"
#include "protocol.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Runtime state of a \c SyncWorker.
    enum class SyncWorkerState {
        Stopped,  ///< No worker thread is running.
        Starting, ///< Worker thread has been created but has not entered the loop.
        Idle,     ///< Worker is sleeping between sync rounds.
        Pulling,  ///< Worker is waiting for \c ISyncPeer::pull().
        Applying, ///< Worker is applying a pulled page locally.
        Backoff,  ///< Last round failed; worker is waiting before retry.
        Stopping, ///< Stop has been requested; current operation is draining.
        Failed,   ///< Worker thread exited after an unexpected exception.
    };

    /// \brief Timing and pagination settings for \c SyncWorker.
    struct SyncWorkerOptions {
        /// \brief Max batches requested from the peer per pull page.
        std::uint64_t max_batches = 1000;

        /// \brief Max encoded batch bytes requested from the peer per pull page.
        std::uint64_t max_bytes = 4ULL * 1024ULL * 1024ULL;

        /// \brief Delay between successful background sync rounds.
        std::chrono::milliseconds idle_interval =
            std::chrono::milliseconds(1000);

        /// \brief Initial delay after a failed background sync round.
        std::chrono::milliseconds initial_backoff =
            std::chrono::milliseconds(100);

        /// \brief Maximum delay after repeated failed background sync rounds.
        std::chrono::milliseconds max_backoff =
            std::chrono::milliseconds(5000);

        /// \brief Whether one round drains all \c has_more pages immediately.
        bool drain_pages = true;
    };

    /// \brief Result of one \c SyncWorker sync round.
    struct SyncWorkerRoundResult {
        bool        ok = true; ///< Whether the round completed without error.
        std::size_t pages_pulled = 0; ///< Number of successful pull pages.
        std::size_t batches_applied = 0; ///< Number of batches applied locally.
        bool        has_more = false; ///< Whether the peer reported more pages.
        std::string error; ///< Failure detail when \c ok is false.
    };

    /// \brief Drives incremental pull/apply sync in a caller-owned lifecycle.
    /// \details The worker owns only its std::thread. It does not own the
    /// supplied \c SyncEngine or \c ISyncPeer; both must outlive the worker.
    /// Lifecycle mutations (\c start(), \c stop(), \c join(), \c run_once())
    /// must be serialized by the caller. Observers and stop signalling
    /// (\c state(), \c last_error(), \c wait_until_state(),
    /// \c request_stop()) are thread-safe.
    ///
    /// The implementation never keeps a local MDBX transaction open while
    /// waiting in \c ISyncPeer::pull(), idle sleep, or backoff sleep. Pulled
    /// batches are applied through \c SyncEngine::handle_push(), which opens
    /// and commits a short local write transaction for each pulled page.
    /// Stop requests do not interrupt a blocking peer call; \c stop(),
    /// \c join(), and the destructor may wait until the peer returns.
    class SyncWorker {
    public:
        /// \brief Constructs a worker over a local engine and remote peer.
        /// \param engine Local engine that applies pulled batches.
        /// \param peer Remote peer that serves pull requests.
        /// \param options Worker timing and pagination options.
        SyncWorker(SyncEngine& engine,
                   ISyncPeer& peer,
                   const SyncWorkerOptions& options = SyncWorkerOptions())
            : m_engine(engine),
              m_peer(peer),
              m_options(options),
              m_state(SyncWorkerState::Stopped),
              m_stop_requested(false) {
            validate_options(m_options);
        }

        /// \brief Requests stop and waits for the background worker to finish.
        /// \details May block until an in-flight \c ISyncPeer::pull() returns.
        ~SyncWorker() {
            request_stop();
            join();
        }

        SyncWorker(const SyncWorker&) = delete;
        SyncWorker& operator=(const SyncWorker&) = delete;

        /// \brief Starts the background worker thread.
        /// \throws std::logic_error if a worker thread is already running.
        void start() {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_thread.joinable()) {
                    throw std::logic_error("SyncWorker is already running");
                }
                if (m_state != SyncWorkerState::Stopped &&
                    m_state != SyncWorkerState::Failed) {
                    throw std::logic_error("SyncWorker is not stopped");
                }
                m_stop_requested = false;
                m_last_error.clear();
                m_state = SyncWorkerState::Starting;
            }
            m_state_changed.notify_all();
            try {
                m_thread = std::thread(&SyncWorker::thread_main, this);
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stop_requested = true;
                    m_state = SyncWorkerState::Stopped;
                }
                m_state_changed.notify_all();
                throw;
            }
        }

        /// \brief Requests background worker shutdown.
        /// \details Does not interrupt an in-flight peer call. The worker exits
        /// before applying a page returned after stop was requested.
        void request_stop() {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop_requested = true;
                if (m_state != SyncWorkerState::Stopped &&
                    m_state != SyncWorkerState::Failed) {
                    m_state = SyncWorkerState::Stopping;
                }
            }
            m_state_changed.notify_all();
        }

        /// \brief Joins the background worker thread if it is running.
        /// \throws std::logic_error if called from the worker thread.
        void join() {
            if (m_thread.joinable()) {
                if (m_thread.get_id() == std::this_thread::get_id()) {
                    throw std::logic_error("SyncWorker cannot join itself");
                }
                m_thread.join();
            }
        }

        /// \brief Requests stop and joins the background worker thread.
        /// \details May block until an in-flight \c ISyncPeer::pull() returns.
        /// \throws std::logic_error if called from the worker thread.
        void stop() {
            request_stop();
            join();
        }

        /// \brief Runs one pull/apply round on the calling thread.
        /// \return Round result with page and batch counts.
        SyncWorkerRoundResult run_once() {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_thread.joinable()) {
                    throw std::logic_error("SyncWorker is already running");
                }
                if (m_state != SyncWorkerState::Stopped &&
                    m_state != SyncWorkerState::Failed) {
                    throw std::logic_error("SyncWorker is not stopped");
                }
                m_stop_requested = false;
                m_last_error.clear();
            }
            const SyncWorkerRoundResult result = run_once_impl();
            if (stop_requested()) {
                set_state(SyncWorkerState::Stopped);
                return result;
            }
            if (result.ok) {
                set_state(SyncWorkerState::Stopped);
            } else {
                set_last_error(result.error);
                set_state(SyncWorkerState::Failed);
            }
            return result;
        }

        /// \brief Returns the current worker state.
        SyncWorkerState state() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_state;
        }

        /// \brief Returns the most recent failure message.
        std::string last_error() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_last_error;
        }

        /// \brief Waits until \p desired is observed or \p timeout expires.
        /// \param desired State to wait for.
        /// \param timeout Maximum wait duration.
        /// \return \c true when \p desired was observed.
        bool wait_until_state(SyncWorkerState desired,
                              std::chrono::milliseconds timeout) const {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_state_changed.wait_for(
                lock, timeout,
                [this, desired] { return m_state == desired; });
        }

    private:
        static void validate_options(const SyncWorkerOptions& options) {
            if (options.max_batches == 0) {
                throw std::invalid_argument(
                    "SyncWorkerOptions::max_batches must be greater than zero");
            }
            if (options.max_bytes == 0) {
                throw std::invalid_argument(
                    "SyncWorkerOptions::max_bytes must be greater than zero");
            }
            if (options.idle_interval < std::chrono::milliseconds::zero()) {
                throw std::invalid_argument(
                    "SyncWorkerOptions::idle_interval must not be negative");
            }
            if (options.initial_backoff < std::chrono::milliseconds::zero()) {
                throw std::invalid_argument(
                    "SyncWorkerOptions::initial_backoff must not be negative");
            }
            if (options.max_backoff < std::chrono::milliseconds::zero()) {
                throw std::invalid_argument(
                    "SyncWorkerOptions::max_backoff must not be negative");
            }
            if (options.max_backoff < options.initial_backoff) {
                throw std::invalid_argument(
                    "SyncWorkerOptions::max_backoff must be at least initial_backoff");
            }
        }

        SyncWorkerRoundResult run_once_impl() {
            SyncWorkerRoundResult result;
            try {
                PullRequest request;
                request.requester = m_engine.local_node_id();
                request.db_id = m_engine.db_uuid();
                request.have = m_engine.applied_cursor();
                request.max_batches = m_options.max_batches;
                request.max_bytes = m_options.max_bytes;

                bool has_more = false;
                do {
                    if (stop_requested()) {
                        result.has_more = has_more;
                        return result;
                    }

                    const SyncCursor before = request.have;
                    set_state(SyncWorkerState::Pulling);
                    const PullResponse response = m_peer.pull(request);
                    if (!response.ok) {
                        result.ok = false;
                        result.error = response.error.empty()
                            ? "pull failed"
                            : response.error;
                        return result;
                    }
                    ++result.pages_pulled;

                    has_more = response.has_more;
                    if (stop_requested()) {
                        result.has_more = has_more;
                        return result;
                    }

                    if (!response.batches.empty()) {
                        set_state(SyncWorkerState::Applying);
                        PushRequest apply;
                        apply.db_id = request.db_id;
                        apply.batches = response.batches;
                        const PushResponse applied = m_engine.handle_push(apply);
                        if (!applied.ok) {
                            result.ok = false;
                            result.error = applied.error.empty()
                                ? "apply failed"
                                : applied.error;
                            return result;
                        }
                        result.batches_applied += response.batches.size();
                    } else if (response.has_more) {
                        result.ok = false;
                        result.error = "pull reported has_more without batches";
                        return result;
                    }

                    request.have = m_engine.applied_cursor();
                    if (has_more &&
                        request.have.last_seq_by_origin == before.last_seq_by_origin) {
                        result.ok = false;
                        result.error = "pull pagination made no cursor progress";
                        return result;
                    }
                    result.has_more = has_more;
                } while (has_more && m_options.drain_pages);
            } catch (const std::exception& e) {
                result.ok = false;
                result.error = e.what();
            } catch (...) {
                result.ok = false;
                result.error = "unknown sync worker error";
            }
            return result;
        }

        void thread_main() {
            std::chrono::milliseconds backoff = m_options.initial_backoff;
            try {
                set_state(SyncWorkerState::Idle);
                while (!stop_requested()) {
                    const SyncWorkerRoundResult result = run_once_impl();
                    if (stop_requested()) {
                        break;
                    }
                    if (result.ok) {
                        backoff = m_options.initial_backoff;
                        set_state(SyncWorkerState::Idle);
                        if (wait_for_stop(m_options.idle_interval)) {
                            break;
                        }
                    } else {
                        set_last_error(result.error);
                        set_state(SyncWorkerState::Backoff);
                        if (wait_for_stop(backoff)) {
                            break;
                        }
                        backoff = next_backoff(backoff);
                    }
                }
                set_state(SyncWorkerState::Stopped);
            } catch (const std::exception& e) {
                set_last_error(e.what());
                set_state(SyncWorkerState::Failed);
            } catch (...) {
                set_last_error("unknown sync worker thread error");
                set_state(SyncWorkerState::Failed);
            }
        }

        bool wait_for_stop(std::chrono::milliseconds duration) const {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_state_changed.wait_for(
                lock, duration,
                [this] { return m_stop_requested; });
        }

        std::chrono::milliseconds next_backoff(
                std::chrono::milliseconds current) const {
            if (current >= m_options.max_backoff) {
                return m_options.max_backoff;
            }
            if (current.count() >= m_options.max_backoff.count() / 2) {
                return m_options.max_backoff;
            }
            return current + current;
        }

        bool stop_requested() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_stop_requested;
        }

        void set_state(SyncWorkerState state) const {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_state = state;
            }
            m_state_changed.notify_all();
        }

        void set_last_error(const std::string& error) const {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_last_error = error;
        }

        SyncEngine&                 m_engine;
        ISyncPeer&                  m_peer;
        SyncWorkerOptions           m_options;
        std::thread                 m_thread;
        mutable std::mutex          m_mutex;
        mutable std::condition_variable m_state_changed;
        mutable SyncWorkerState     m_state;
        mutable bool                m_stop_requested;
        mutable std::string         m_last_error;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_SYNC_WORKER_HPP_INCLUDED
