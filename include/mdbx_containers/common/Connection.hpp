#pragma once
#ifndef _MDBX_CONTAINERS_CONNECTION_HPP_INCLUDED
#define _MDBX_CONTAINERS_CONNECTION_HPP_INCLUDED

/// \file Connection.hpp
/// \brief Manages an MDBX database connection using a provided configuration.

namespace mdbxc {

    /// \class Connection
    /// \ingroup mdbxc_core
    /// \brief Manages a single MDBX environment and an optional read-only transaction.
    class Connection : private TransactionTracker {
        friend class BaseTable;
    public:

        /// \brief Default constructor.
        Connection() = default;

        /// \brief Constructs a connection using the given MDBX configuration.
        /// \param config Configuration used to initialize the environment.
        explicit Connection(const Config& config);

        /// \brief Destructor. Closes the MDBX environment and aborts any open transactions.
        ~Connection();
        
        /// \brief Creates and connects a new shared Connection instance.
        /// \param config Configuration to use for initialization.
        /// \return Shared pointer to the created Connection.
        static std::shared_ptr<Connection> create(const Config& config);

        /// \brief Sets the MDBX configuration (must be called before connect()).
        /// \param config New configuration to apply.
        void configure(const Config& config);

        /// \brief Connects to the database using the current configuration.
        /// \throws MdbxException on configuration or environment errors.
        void connect();
        
        /// \brief Sets configuration and connects in one step.
        /// \param config Configuration to use.
        void connect(const Config& config);

        /// \brief Disconnects from the MDBX environment and releases resources.
        /// \throws MdbxException if closing the environment fails.
        void disconnect();

        /// \brief Checks whether the environment is currently open.
        /// \return true if connected, false otherwise.
        bool is_connected() const;

        /// \brief Creates a RAII transaction object.
        /// \param mode Transaction mode to open (default: WRITABLE).
        /// \throws MdbxException on MDBX errors.
        /// \return Transaction guard managing the MDBX_txn handle.
        Transaction transaction(TransactionMode mode = TransactionMode::WRITABLE);
        
        /// \brief Begins a manual transaction (must be committed or rolled back later).
        /// \param mode The transaction mode (default: WRITABLE).
        /// \throws MdbxException if the transaction is already started or if beginning fails.
        /// \throws std::logic_error
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

    private:
        friend class Transaction;

        MDBX_env *m_env = nullptr;          ///< Pointer to the MDBX environment handle.
        mutable std::mutex m_mdbx_mutex;    ///< Mutex for thread-safe access.
        std::unordered_map<std::thread::id, std::shared_ptr<Transaction>> m_transactions;
#       if __cplusplus >= 201703L
        using config_t = std::optional<Config>;
#       else
        using config_t = std::unique_ptr<Config>;
#       endif
        config_t m_config;                  ///< Database configuration object.

        /// \brief Initializes the environment and sets up read-only transaction.
        void initialize();
        
        /// \brief Safely closes the environment and aborts transaction if needed.
        /// \param use_throw If true, throw on error; otherwise suppress exceptions.
        void cleanup(bool use_throw = true);

        /// \brief Initializes MDBX environment and opens a read-only transaction.
        void db_init();

    }; // Connection

} // namespace mdbxc

#ifdef MDBX_CONTAINERS_HEADER_ONLY
#include "Connection.ipp"
#endif

#endif // _MDBX_CONTAINERS_CONNECTION_HPP_INCLUDED
