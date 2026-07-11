#pragma once
#ifndef MDBX_CONTAINERS_HEADER_DETAIL_BASE_TABLE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_DETAIL_BASE_TABLE_HPP_INCLUDED

/// \file BaseTable.hpp
/// \brief Base class for working with MDBX databases (tables).

#include <cstdint>

#ifndef MDBXC_SYNC_ENABLED
#define MDBXC_SYNC_ENABLED 0
#endif

#include <string>
#include <vector>

namespace mdbxc {
    
    /// \class BaseTable
    /// \ingroup mdbxc_core
    /// \brief Base class providing common functionality for MDBX database access.
    ///
    /// Opens or creates a table (DBI handle) and offers basic transaction management.
    /// In read-only connections, opens an existing DBI in a read-only
    /// transaction and ignores `MDBX_CREATE`.
    ///
    /// \thread_safety Not thread-safe for simultaneous operations on the same
    /// instance. For multi-threaded code, prefer separate table wrapper
    /// instances per worker thread over the same shared Connection, or protect
    /// one shared wrapper instance with an external mutex.
    ///
    /// \note The DBI handle may be used by separate thread-owned transactions
    /// through separate wrapper instances while the shared Connection lifecycle
    /// remains stable.
    class BaseTable {
    public:
        /// \brief Construct the database table accessor.
        /// \param connection Shared MDBX connection.
        /// \param name Name of the table (used as DBI name).
        /// \param flags DBI flags (e.g., MDBX_CREATE).
        explicit BaseTable(std::shared_ptr<Connection> connection,
                        std::string name,
                        MDBX_db_flags_t flags)
            : m_connection(std::move(connection)), m_name(std::move(name)) {
            bool read_only = m_connection->is_read_only();
            MDBX_db_flags_t open_flags = read_only
                ? static_cast<MDBX_db_flags_t>(flags & ~MDBX_CREATE)
                : flags;
            auto txn = m_connection->transaction(
                read_only ? TransactionMode::READ_ONLY : TransactionMode::WRITABLE
            );
            check_mdbx(
                mdbx_dbi_open(txn.handle(), m_name.c_str(), open_flags, &m_dbi),
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
        /// \warning Lifecycle-only. Do not call concurrently with table
        /// operations or active transactions.
        void connect() {
            m_connection->connect();
        }

        /// \brief Disconnects the MDBX environment.
        /// \throws MdbxException if closing the environment fails.
        /// \warning Lifecycle-only. Do not call concurrently with table
        /// operations or active transactions.
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

        /// \brief Helper that opens (or reuses) a transaction for \p action.
        ///
        /// \details Lifecycle pattern expected by every table operation:
        /// \code
        ///   auto conn = Connection::create(config);
        ///   KeyValueTable<int, User> users(conn, "users");
        ///   // ... declare more tables ...
        ///   auto txn = conn->transaction(TransactionMode::WRITABLE);
        ///   users.insert_or_assign(id, user, txn.handle());
        ///   // ... more ops inside the same txn ...
        ///   txn.commit();
        /// \endcode
        ///
        /// Calling a table method with the default-arg path (no explicit
        /// \p txn) is only safe when the thread is not already inside a
        /// writable transaction. If it is, this helper throws rather
        /// than silently re-using the thread-bound transaction. Silent
        /// reuse hides the scope of the active transaction and makes
        /// the lifetime of \c m_dbi handles harder to reason about;
        /// explicit is better than implicit.
        ///
        /// The error message names the missing argument so the fix is
        /// obvious: pass the active transaction handle to the operation.
        template<typename F>
        void with_transaction(F&& action, TransactionMode mode, MDBX_txn* txn = nullptr) const {
            if (txn) {
                action(txn);
                return;
            }
            if (MDBX_txn* current = m_connection->thread_txn(); current != nullptr) {
                throw std::logic_error(
                    "mdbx_containers: a transaction is already active on this "
                    "connection's thread. Pass it explicitly to the table "
                    "operation, e.g. kv.insert_or_assign(k, v, txn).");
            }
            auto txn_guard = m_connection->transaction(mode);
            try {
                action(txn_guard.handle());
                txn_guard.commit();
            } catch (...) {
                try { txn_guard.rollback(); } catch (...) {}
                throw;
            }
        }

        struct CursorGuard {
            MDBX_cursor* cursor;

            CursorGuard() noexcept : cursor(nullptr) {}
            explicit CursorGuard(MDBX_cursor* cursor_handle) noexcept : cursor(cursor_handle) {}
            ~CursorGuard() noexcept { close(); }

            MDBX_cursor** out() noexcept { return &cursor; }
            MDBX_cursor* get() const noexcept { return cursor; }

            void close() noexcept {
                if (cursor) {
                    mdbx_cursor_close(cursor);
                    cursor = nullptr;
                }
            }

            CursorGuard(const CursorGuard&) = delete;
            CursorGuard& operator=(const CursorGuard&) = delete;
            CursorGuard(CursorGuard&&) = delete;
            CursorGuard& operator=(CursorGuard&&) = delete;
        };

        std::shared_ptr<Connection>  m_connection;   ///< Shared connection to MDBX environment.
        MDBX_dbi                     m_dbi{};         ///< DBI handle for the opened table.
        std::string                  m_name;          ///< DBI name (used for sync capture).

        /// \brief Returns the transaction bound to the current thread, if any.
        /// \return Pointer to the MDBX transaction or nullptr.
        MDBX_txn* thread_txn() const {
                return m_connection->thread_txn();
        }
        
        /// \brief Gets the raw DBI handle.
        /// \return DBI handle for the opened table.
        MDBX_dbi handle() const { return m_dbi; }

        /// \brief Throws when a duplicate value exceeds the configured proactive limit.
        /// \param value Duplicate value that will be written into an MDBX_DUPSORT DBI.
        void check_dupsort_value_size(const MDBX_val& value) const {
            const int64_t limit = m_connection->max_dupsort_value_size();
            if (limit > 0 && value.iov_len > static_cast<std::size_t>(limit)) {
                throw std::length_error(
                    "MDBX_DUPSORT duplicate value is too large. "
                    "Use a large-value layout or increase Config::max_dupsort_value_size."
                );
            }
        }

#       if MDBXC_SYNC_ENABLED
        /// \brief Forwards a successful write to the attached sync capture sink.
        /// \details Called from derived table \c db_* helpers right after a
        /// successful \c mdbx_put / \c mdbx_del. No-op when no sink is attached
        /// or when the connection is read-only.
        void record_op(MDBX_txn* txn,
                       sync::ChangeOpType op_type,
                       const std::vector<std::uint8_t>& storage_key,
                       const std::vector<std::uint8_t>& value) const {
            if (m_connection->is_read_only()) return;
            sync::ISyncCaptureSink* sink = m_connection->sync_capture();
            if (sink == nullptr) return;
            sink->record_change(txn, m_name, op_type, storage_key, value);
        }
#       endif
    };
    
}; // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_DETAIL_BASE_TABLE_HPP_INCLUDED
