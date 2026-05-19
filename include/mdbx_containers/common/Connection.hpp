#pragma once
#ifndef _MDBX_CONTAINERS_CONNECTION_HPP_INCLUDED
#define _MDBX_CONTAINERS_CONNECTION_HPP_INCLUDED

/// \file Connection.hpp
/// \brief Manages an MDBX database connection using a provided configuration.

namespace mdbxc {

    /// \class Connection
    /// \ingroup mdbxc_core
    /// \brief Manages a single MDBX environment and per-thread transaction tracking.
    ///
    /// \thread_safety Conditionally thread-safe: table operations may run on
    /// different threads while the connection lifecycle is stable and every
    /// thread uses only its own transaction handles. Lifecycle operations
    /// (`configure()`, `connect()`, `disconnect()`, and destruction) must not
    /// run concurrently with table operations or active transactions. Use
    /// `shutdown()` or `shutdown_for()` to request a coordinated stop.
    ///
    /// \note Internal mutexes protect connection state and manual transaction
    /// registries. They do not make `MDBX_txn*` or MDBX cursors safe to use
    /// from another thread.
    /// \see https://libmdbx.dqdkfa.ru/group__c__transactions.html
    /// \see https://libmdbx.dqdkfa.ru/group__c__opening.html
    class Connection : private TransactionTracker {
        friend class BaseTable;
    public:

        /// \brief Default constructor.
        Connection() = default;

        /// \brief Constructs a connection using the given MDBX configuration.
        /// \param config Configuration used to initialize the environment.
        explicit Connection(const Config& config);

        /// \brief Destructor. Closes the MDBX environment when no transaction handle is open.
        ///
        /// \warning Lifecycle-only. Destroy the connection after all worker
        /// threads have stopped and all transactions/cursors using the
        /// environment are gone.
        ~Connection();
        
        /// \brief Creates and connects a new shared Connection instance.
        /// \param config Configuration to use for initialization.
        /// \return Shared pointer to the created Connection.
        static std::shared_ptr<Connection> create(const Config& config);

        /// \brief Sets the MDBX configuration (must be called before connect()).
        /// \param config New configuration to apply.
        /// \warning Lifecycle-only. Do not call concurrently with table
        /// operations or active transactions.
        void configure(const Config& config);

        /// \brief Connects to the database using the current configuration.
        /// \throws MdbxException on configuration or environment errors.
        /// \warning Lifecycle-only. Call before concurrent table activity.
        void connect();
        
        /// \brief Sets configuration and connects in one step.
        /// \param config Configuration to use.
        /// \warning Lifecycle-only. Call before concurrent table activity.
        void connect(const Config& config);

        /// \brief Disconnects from the MDBX environment and releases resources.
        /// \throws MdbxException if closing the environment fails.
        /// \throws MdbxException with MDBX_BUSY if any transaction handle is open.
        /// \warning Lifecycle-only. Call after all concurrent table activity,
        /// transactions, and MDBX cursors have ended. This method does not wait
        /// for worker threads and does not abort transactions owned by another
        /// thread. Use `shutdown()`/`shutdown_for()` for coordinated closing.
        void disconnect();

        /// \brief Requests shutdown, waits for transaction handles, then disconnects.
        /// \throws std::logic_error if the calling thread owns a transaction handle.
        /// \throws MdbxException if MDBX close fails.
        ///
        /// New transactions are rejected after shutdown is requested. Existing
        /// transaction handles must be closed on their owning threads.
        void shutdown();

        /// \brief Requests shutdown and waits up to \p timeout before disconnecting.
        /// \param timeout Maximum time to wait for open transaction handles.
        /// \return true if the environment was disconnected, false on timeout.
        /// \throws std::logic_error if the calling thread owns a transaction handle.
        /// \throws MdbxException if MDBX close fails after transactions finish.
        ///
        /// When this returns false, shutdown remains requested: new transactions
        /// are still rejected, and the caller may retry `shutdown_for()` or
        /// `shutdown()` after worker threads finish.
        template<class Rep, class Period>
        bool shutdown_for(const std::chrono::duration<Rep, Period>& timeout) {
            if (!request_shutdown()) {
                return true;
            }
            if (!wait_for_no_txn_handles_for(timeout)) {
                return false;
            }
            disconnect();
            return true;
        }

        /// \brief Checks whether the environment is currently open.
        /// \return true if connected, false otherwise.
        bool is_connected() const;

        /// \brief Checks whether the environment is configured read-only.
        /// \return true if the current configuration opens MDBX with MDBX_RDONLY.
        bool is_read_only() const;

        /// \brief Creates a RAII transaction object.
        /// \param mode Transaction mode to open (default: WRITABLE).
        /// \throws MdbxException on MDBX errors.
        /// \return Transaction guard managing the MDBX_txn handle.
        /// \warning The returned transaction belongs to the calling thread.
        /// Do not use or destroy it from another thread.
        Transaction transaction(TransactionMode mode = TransactionMode::WRITABLE);
        
        /// \brief Begins a manual transaction (must be committed or rolled back later).
        /// \param mode The transaction mode (default: WRITABLE).
        /// \throws MdbxException if the transaction is already started or if beginning fails.
        /// \throws std::logic_error
        /// \warning Manual transactions are bound to the calling thread.
        void begin(TransactionMode mode = TransactionMode::WRITABLE);
        
        /// \brief Commits the current manual transaction.
        /// \throws MdbxException if no transaction is active or commit/reset fails.
        void commit();

        /// \brief Rolls back the current manual transaction.
        /// \throws MdbxException if no transaction is active or rollback/reset fails.
        void rollback();
        
        /// \brief Returns the transaction associated with the current thread.
        /// \return Shared pointer to the active Transaction or nullptr.
        std::shared_ptr<Transaction> current_txn() const;

        /// \brief Returns the environment handle.
        /// \return MDBX environment pointer.
        MDBX_env* env_handle() noexcept;

        /// \brief Returns the configured proactive DUPSORT duplicate value limit.
        /// \return Maximum duplicate value size, or a non-positive value when disabled.
        int64_t max_dupsort_value_size() const;

    private:
        friend class Transaction;

        MDBX_env *m_env = nullptr;          ///< Pointer to the MDBX environment handle.
        mutable std::mutex m_mdbx_mutex;    ///< Protects lifecycle state and manual transaction map.
        std::unordered_map<std::thread::id, std::shared_ptr<Transaction>> m_transactions;
        bool m_shutdown_requested = false;  ///< Rejects new transactions during coordinated shutdown.
#       if __cplusplus >= 201703L
        using config_t = std::optional<Config>;
#       else
        using config_t = std::unique_ptr<Config>;
#       endif
        config_t m_config;                  ///< Database configuration object.

        /// \brief Initializes the MDBX environment.
        void initialize();
        
        /// \brief Safely closes the environment and aborts tracked transactions if needed.
        /// \param use_throw If true, throw on error; otherwise suppress exceptions.
        void cleanup(bool use_throw = true);

        /// \brief Marks the connection as shutting down.
        /// \return true if the connection is open and needs waiting/closing.
        bool request_shutdown();

        /// \brief Initializes and opens the MDBX environment.
        void db_init();

    }; // Connection

} // namespace mdbxc

#ifdef MDBX_CONTAINERS_HEADER_ONLY
#include "Connection.ipp"
#endif

#endif // _MDBX_CONTAINERS_CONNECTION_HPP_INCLUDED
