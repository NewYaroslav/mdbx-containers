#pragma once
#ifndef _MDBX_CONTAINERS_TRANSACTION_TRACKER_HPP_INCLUDED
#define _MDBX_CONTAINERS_TRANSACTION_TRACKER_HPP_INCLUDED

/// \file TransactionTracker.hpp
/// \brief Tracks MDBX transactions per thread for reuse and cleanup.

namespace mdbxc {

    /// \class TransactionTracker
    /// \ingroup mdbxc_core
    /// \brief Associates MDBX transactions with threads.
    ///
    /// Manages a map from thread IDs to MDBX transaction pointers,
    /// allowing reuse and cleanup of transactions for specific threads.
    class TransactionTracker {
        friend class Transaction;
    protected:

        /// \brief Registers a transaction for a specific thread.
        /// \param txn Pointer to the MDBX transaction.
        void bind_txn(MDBX_txn* txn);

        /// \brief Unregisters a transaction for a specific thread.
        void unbind_txn();

        /// \brief Retrieves the transaction associated with the current thread.
        /// \return Pointer to the MDBX transaction, or nullptr if not found.
        MDBX_txn* thread_txn() const;
        
        virtual ~TransactionTracker() = default;

    private:
        mutable std::mutex m_mutex;  ///< Protects access to m_thread_txns.
        std::unordered_map<std::thread::id, MDBX_txn*> m_thread_txns; ///< Map of thread IDs to transaction pointers.
    };

} // namespace mdbxc

#ifdef MDBX_CONTAINERS_HEADER_ONLY
#include "TransactionTracker.ipp"
#endif

#endif // _MDBX_CONTAINERS_TRANSACTION_TRACKER_HPP_INCLUDED