#pragma once
#ifndef _MDBX_CONTAINERS_TRANSACTION_HPP_INCLUDED
#define _MDBX_CONTAINERS_TRANSACTION_HPP_INCLUDED

/// \file Transaction.hpp
/// \brief Declares the Transaction class, a wrapper for managing MDBX transactions.

namespace mdbxc {
    
    /// \enum TransactionMode
    /// \brief Specifies the access mode of a transaction.
    ///
    /// Defines whether the transaction is read-only or writable.
    enum class TransactionMode {
        READ_ONLY,  ///< Read-only transaction (no write operations allowed).
        WRITABLE    ///< Writable transaction (allows inserts, updates, deletes).
    };

    /// \class Transaction
    /// \brief Manages MDBX transactions with automatic cleanup and error handling.
    ///
    /// Supports both read-only and writable modes. Provides methods for beginning,
    /// committing, and rolling back transactions, with integration of MDBX-specific behavior.
    class Transaction {
        friend class Connection;
    public:

        /// \brief Destructor that safely closes or resets the transaction.
        ///
        /// If the transaction is still active, read-only transactions are reset (reusable),
        /// and writable transactions are aborted.
        virtual ~Transaction();

        /// \brief Starts the transaction.
        ///
        /// For read-only transactions, uses a shared reusable handle and attempts renewal.
        /// For writable transactions, begins a new transaction using the MDBX environment.
        ///
        /// \throws MdbxException if the transaction is already started or if beginning fails.
        void begin();

        /// \brief Commits the transaction.
        ///
        /// For read-only transactions, resets the handle for reuse.
        /// For writable transactions, commits the changes and closes the handle.
        ///
        /// \throws MdbxException if no transaction is active or commit/reset fails.
        void commit();

        /// \brief Rolls back the transaction.
        ///
        /// For read-only transactions, resets the handle.
        /// For writable transactions, aborts the transaction.
        ///
        /// \throws MdbxException if no transaction is active or rollback/reset fails.
        void rollback();

        /// \brief Returns the internal MDBX transaction handle.
        /// \return Raw pointer to MDBX_txn, or nullptr if not active.
        MDBX_txn *handle() const noexcept;
        
        /// \brief Constructs a new transaction object.
        /// \param env Pointer to the MDBX environment handle.
        /// \param mode Access mode of the transaction.
        Transaction(TransactionTracker* registry, MDBX_env* env, TransactionMode mode);

    private:

        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        TransactionTracker* m_registry = nullptr;
        MDBX_env*       m_env = nullptr;            ///< Pointer to the MDBX environment handle.
        MDBX_txn*       m_txn = nullptr;            ///< MDBX transaction handle.
        TransactionMode m_mode;                     ///< Current transaction mode.
        bool            m_started = false;
    }; // Transaction

}; // namespace mdbxc

#ifdef MDBX_CONTAINERS_HEADER_ONLY
#include "Transaction.ipp"
#endif

#endif // _MDBX_CONTAINERS_TRANSACTION_HPP_INCLUDED
