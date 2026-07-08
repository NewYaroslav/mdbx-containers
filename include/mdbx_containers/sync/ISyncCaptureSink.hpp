#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_I_SYNC_CAPTURE_SINK_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_I_SYNC_CAPTURE_SINK_HPP_INCLUDED

/// \file ISyncCaptureSink.hpp
/// \brief Bridge between \c Connection / \c BaseTable writes and the sync
/// subsystem's per-transaction change recorder.
/// \details
/// \c Connection holds a non-owning pointer to an \c ISyncCaptureSink set via
/// \c Connection::attach_sync_capture(). \c BaseTable::record_op() forwards
/// every successful write through this sink. \c Transaction::commit() calls
/// \c flush_in_txn() on the same write transaction so the captured batch is
/// written to \c _mdbxc_changelog atomically with the user-visible change.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "SyncModule.hpp"

#if MDBXC_SYNC_ENABLED
#include <mdbx.h>

#include "ChangeOp.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Interface between mdbxc-core write paths and the sync recorder.
    /// \details The implementation owns thread-local pending state, decides
    /// when to assemble a batch, and writes it to \c _mdbxc_changelog inside
    /// the same write transaction that performed the user-visible change.
    class ISyncCaptureSink {
    public:
        virtual ~ISyncCaptureSink() = default;

        /// \brief Called by \c BaseTable after a successful \c mdbx_put /
        /// \c mdbx_del on a user table.
        /// \param txn The active MDBX write transaction that performed the
        ///        change. Implementations may stage the op in thread-local
        ///        memory and defer the on-disk write to \c flush_in_txn.
        /// \param dbi_name Name of the user table (the DBI name as passed
        ///        to the table constructor).
        /// \param op_type Kind of write (put/delete/clear).
        /// \param storage_key Serialized MDBX key bytes of the touched record.
        /// \param value Serialized MDBX value bytes for \c Put; empty for
        ///        \c Delete / \c ClearTable.
        virtual void record_change(MDBX_txn* txn,
                                   const std::string& dbi_name,
                                   ChangeOpType op_type,
                                   const std::vector<std::uint8_t>& storage_key,
                                   const std::vector<std::uint8_t>& value) = 0;

        /// \brief Called by \c Transaction::commit() before the actual commit.
        /// \param txn The about-to-commit write transaction.
        /// \details Implementations must write any pending captured changes to
        /// \c _mdbxc_changelog within \p txn so that user-visible writes and
        /// their changelog entry land or fail atomically together.
        virtual void flush_in_txn(MDBX_txn* txn) = 0;

        /// \brief Discards any pending ops recorded for a transaction that
        /// is about to be aborted or rolled back.
        /// \param txn The about-to-be-aborted write transaction.
        /// \details Default implementation is a no-op; overloads drop the
        /// pending ops so the next transaction on the same thread (or the
        /// next MDBX_txn* address if the allocator reuses it) starts clean.
        virtual void discard_txn(MDBX_txn* txn) noexcept {
            (void)txn;
        }
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_I_SYNC_CAPTURE_SINK_HPP_INCLUDED
#endif // MDBXC_SYNC_ENABLED guard