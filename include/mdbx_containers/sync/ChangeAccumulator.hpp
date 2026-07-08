#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_CHANGE_ACCUMULATOR_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_CHANGE_ACCUMULATOR_HPP_INCLUDED

/// \file ChangeAccumulator.hpp
/// \brief Default \c ISyncCaptureSink that buffers per-transaction ops and
/// writes a single \c ChangeBatch to \c _mdbxc_changelog on flush.
/// \details
/// Pending ops are kept in a \c std::unordered_map keyed by the
/// \c MDBX_txn* pointer of the about-to-commit write transaction. A thread
/// may have multiple distinct write transactions in different RAII guards;
/// the pointer key keeps their pending lists separate, so a transaction
/// that aborts (or whose on_pre_commit throws) does not leak its ops into
/// the next transaction on the same thread.
///
/// On \c flush_in_txn the accumulator:
///  1. moves the pending ops for this \p txn out of the map under one
///     mutex acquisition;
///  2. opens the system stores \c _mdbxc_meta and \c _mdbxc_changelog on
///     first use, then resolves the local \c node_id from \c _mdbxc_meta
///     and refuses to flush when \c node_id is still the all-zero
///     placeholder (the \c SyncEngine must initialise it before capture
///     is enabled);
///  3. builds one \c ChangeBatch, increments \c local_seq on \c _mdbxc_meta,
///     encodes the batch and appends it to \c _mdbxc_changelog inside the
///     same write transaction;
///  4. on any exception, restores the moved-out ops back into the map keyed
///     by \p txn so a retry of the same write transaction can re-emit them;
///  5. on \c discard_txn, drops the pending ops for an aborted transaction
///     so a future commit on the same address (allocator reuse) cannot pick
///     them up as if they were committed.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "SyncModule.hpp"

#if MDBXC_SYNC_ENABLED
#include <mdbx.h>

#include "../common/Connection.hpp"
#include "../common/MdbxException.hpp"
#include "ChangeBatch.hpp"
#include "ChangeBatchCodec.hpp"
#include "ChangeOp.hpp"
#include "Common.hpp"
#include "ISyncCaptureSink.hpp"
#include "stores/ChangeLogStore.hpp"
#include "stores/MetaStore.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Default \c ISyncCaptureSink implementation.
    class ThreadLocalChangeAccumulator : public ISyncCaptureSink {
    public:
        /// \brief Constructs an accumulator bound to \p conn.
        /// \param conn Shared connection whose \c _mdbxc_changelog and
        ///        \c _mdbxc_meta DBIs will be opened lazily on first flush.
        explicit ThreadLocalChangeAccumulator(std::shared_ptr<Connection> conn)
            : m_conn(std::move(conn)),
              m_meta(m_conn->env_handle()),
              m_change_log(m_conn->env_handle()) {}

        void record_change(MDBX_txn* txn,
                           const std::string& dbi_name,
                           ChangeOpType op_type,
                           const std::vector<std::uint8_t>& storage_key,
                           const std::vector<std::uint8_t>& value) override {
            PendingOp op;
            op.op_type = op_type;
            op.dbi_name = dbi_name;
            op.storage_key = storage_key;
            op.value = value;
            std::lock_guard<std::mutex> lk(m_mutex);
            m_pending[txn].push_back(std::move(op));
        }

        void flush_in_txn(MDBX_txn* txn) override {
            std::vector<PendingOp> ops;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_pending.find(txn);
                if (it == m_pending.end() || it->second.empty()) {
                    return;
                }
                ops.swap(it->second);
            }

            try {
                open_stores(txn);
                build_and_append_batch(txn, ops);
                std::lock_guard<std::mutex> lk(m_mutex);
                m_pending.erase(txn);
            } catch (...) {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_pending[txn] = std::move(ops);
                throw;
            }
        }

    private:
        struct PendingOp {
            ChangeOpType op_type = ChangeOpType::Put;
            std::string dbi_name;
            std::vector<std::uint8_t> storage_key;
            std::vector<std::uint8_t> value;
        };

        void open_stores(MDBX_txn* txn) {
            /// \note Each flush_in_txn call must reopen the stores inside the
            /// about-to-commit write transaction. \c m_open is set on first
            /// \c open() and never reset; if a previous flush_in_txn succeeded
            /// and committed, the wrapped \c m_dbi handle refers to a previous
            /// transaction and any subsequent \c mdbx_put/\c mdbx_get on it
            /// returns MDBX_EINVAL. Force-reopen here to keep one accumulator
            /// alive across many transactions.
            m_meta.reset_open();
            m_meta.open(txn);
            m_change_log.reset_open();
            m_change_log.open(txn);
            if (m_node_id == make_zero_node()) {
                NodeId from_meta = m_meta.get_node_id(txn);
                if (compare_node_id(from_meta, make_zero_node()) == 0) {
                    throw MdbxException(
                        "Sync node_id is not initialised. Attach a SyncEngine "
                        "and write _mdbxc_meta.node_id before enabling capture.",
                        MDBX_EINVAL
                    );
                }
                m_node_id = from_meta;
            }
        }

        void build_and_append_batch(MDBX_txn* txn, const std::vector<PendingOp>& ops) {
            ChangeBatch batch;
            batch.version = 1;
            batch.origin_node_id = m_node_id;
            batch.seq = m_meta.increment_local_seq(txn);
            batch.ops.reserve(ops.size());
            for (const PendingOp& op : ops) {
                ChangeOp co;
                co.op_type = op.op_type;
                co.dbi_name = op.dbi_name;
                co.storage_key = op.storage_key;
                co.value = op.value;
                batch.ops.push_back(std::move(co));
            }
            const std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch);
            m_change_log.append(txn, m_node_id, batch.seq, bytes);
        }

        std::shared_ptr<Connection> m_conn;
        NodeId m_node_id{};
        MetaStore m_meta;
        ChangeLogStore m_change_log;
        std::mutex m_mutex;
        std::unordered_map<MDBX_txn*, std::vector<PendingOp>> m_pending;

        void discard_txn(MDBX_txn* txn) noexcept override {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_pending.erase(txn);
        }
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_CHANGE_ACCUMULATOR_HPP_INCLUDED
#endif // MDBXC_SYNC_ENABLED guard