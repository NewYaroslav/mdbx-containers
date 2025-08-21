#pragma once
#ifndef _MDBX_CONTAINERS_BASE_DB_HPP_INCLUDED
#define _MDBX_CONTAINERS_BASE_DB_HPP_INCLUDED

/// \file BaseTable.hpp
/// \brief Base class for working with MDBX databases (tables).

namespace mdbxc {
    
    /// \class BaseTable
    /// \ingroup mdbxc_core
    /// \brief Base class providing common functionality for MDBX database access.
    ///
    /// Opens or creates a table (DBI handle) and offers basic transaction management.
    /// Not thread-safe for simultaneous operations on the same instance.
    class BaseTable {
    public:
        /// \brief Construct the database table accessor.
        /// \param connection Shared MDBX connection.
        /// \param name Name of the table (used as DBI name).
        /// \param flags DBI flags (e.g., MDBX_CREATE).
        explicit BaseTable(std::shared_ptr<Connection> connection,
                        std::string name,
                        MDBX_db_flags_t flags)
            : m_connection(std::move(connection)) {
            auto txn = m_connection->transaction();
            check_mdbx(
                mdbx_dbi_open(txn.handle(), name.c_str(), flags, &m_dbi),
                "Failed to open table"
            );
            txn.commit();
        }
        
        virtual ~BaseTable() = default;
        
        /// \brief Checks if the connection is currently active.
        /// \return true if connected, false otherwise.
        bool is_connected() const {
            return m_connection->is_connected();
        }

        /// \brief Connects to the MDBX environment if not already connected.
        /// \throws MdbxException if connection fails.
        void connect() {
            m_connection->connect();
        }

        /// \brief Disconnects the MDBX environment.
        /// \throws MdbxException if closing the environment fails.
        void disconnect() {
            m_connection->disconnect();
        }
        
        /// \brief Begins a manual transaction (must be committed or rolled back later).
        /// \param mode The transaction mode (default: WRITABLE).
        /// \throws MdbxException if the transaction is already started or if beginning fails.
        void begin(TransactionMode mode = TransactionMode::WRITABLE) {
            m_connection->begin(mode);
        }

        /// \brief Commits the current manual transaction.
        /// \throws MdbxException if no transaction is active or commit/reset fails.
        void commit() {
            m_connection->commit();
        }

        /// \brief Rolls back the current manual transaction.
        /// \throws MdbxException if no transaction is active or rollback/reset fails.
        void rollback() {
            m_connection->rollback();
        }
        
        /// \brief Executes an operation inside an automatic transaction.
        /// \param operation The function to execute.
        /// \param mode The transaction mode (default: WRITABLE).
        /// \throws MdbxException if an error occurs during execution.
        template<typename Func>
        void execute_in_transaction(Func operation, TransactionMode mode = TransactionMode::WRITABLE) {
            auto txn = m_connection->transaction(mode);
            try {
                operation();
                txn.commit();
            } catch(...) {
                try {
                    txn.rollback();
                } catch(...) {}
                throw;
            }
        }

    protected:
        std::shared_ptr<Connection>  m_connection;   ///< Shared connection to MDBX environment.
        MDBX_dbi                     m_dbi{};         ///< DBI handle for the opened table.

        /// \brief Returns the transaction bound to the current thread, if any.
        /// \return Pointer to the MDBX transaction or nullptr.
        MDBX_txn* thread_txn() const {
                return m_connection->thread_txn();
        }
        
        /// \brief Gets the raw DBI handle.
        /// \return DBI handle for the opened table.
        MDBX_dbi handle() const { return m_dbi; }
    };
    
}; // namespace mdbxc

#endif // _MDBX_CONTAINERS_BASE_DB_HPP_INCLUDED

