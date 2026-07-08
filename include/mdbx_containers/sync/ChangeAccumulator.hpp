#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_CHANGE_ACCUMULATOR_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_CHANGE_ACCUMULATOR_HPP_INCLUDED

/// \file ChangeAccumulator.hpp
/// \brief Thread-aware \c ISyncCaptureSink that buffers per-thread ops and
/// writes a single \c ChangeBatch to \c _mdbxc_changelog on flush.
/// \details
/// Pending ops are kept in a \c std::unordered_map keyed by \c std::thread::id
/// and protected by a single \c std::mutex. No \c thread_local STL storage
/// is used; see \c guides/implementation-notes.md for the rationale.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <mdbx.h>

#include "ChangeOp.hpp"
#include "ChangeBatch.hpp"
#include "ChangeBatchCodec.hpp"
#include "ISyncCaptureSink.hpp"
#include "stores/ChangeLogStore.hpp"
#include "stores/MetaStore.hpp"
#include "Common.hpp"
#include "../common/Connection.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Default \c ISyncCaptureSink implementation.
    /// \details Stores pending \c ChangeOp entries per thread under a mutex
    /// and, on \c flush_in_txn, packages them into one \c ChangeBatch and
    /// writes the batch to \c _mdbxc_changelog via \c ChangeLogStore inside
    /// the calling write transaction. Local sequence numbers are issued
    /// monotonically per node via \c increment_local_seq on the bound
    /// \c MetaStore.
    class ThreadLocalChangeAccumulator : public ISyncCaptureSink {
    public:
        /// \brief Constructs an accumulator bound to \p conn.
        /// \param conn Shared connection whose \c _mdbxc_changelog and
        ///        \c _mdbxc_meta DBIs will be opened lazily on first flush.
        explicit ThreadLocalChangeAccumulator(std::shared_ptr<Connection> conn)
            : m_conn(std::move(conn)),
              m_meta(conn->env_handle()),
              m_change_log(conn->env_handle()) {}

        void record_change(MDBX_txn* txn,
                           const std::string& dbi_name,
                           ChangeOpType op_type,
                           const std::vector<std::uint8_t>& storage_key,
                           const std::vector<std::uint8_t>& value) override {
            (void)txn;
            PendingOp op;
            op.op_type = op_type;
            op.dbi_name = dbi_name;
            op.storage_key = storage_key;
            op.value = value;
            const std::thread::id tid = std::this_thread::get_id();
            std::lock_guard<std::mutex> lk(m_mutex);
            m_pending[tid].push_back(std::move(op));
        }

        void flush_in_txn(MDBX_txn* txn) override {
            const std::thread::id tid = std::this_thread::get_id();
            std::vector<PendingOp> ops;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_pending.find(tid);
                if (it == m_pending.end() || it->second.empty()) {
                    return;
                }
                ops.swap(it->second);
            }
            if (!m_change_log.is_open()) {
                open_stores(txn);
            }

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

    private:
        struct PendingOp {
            ChangeOpType op_type = ChangeOpType::Put;
            std::string dbi_name;
            std::vector<std::uint8_t> storage_key;
            std::vector<std::uint8_t> value;
        };

        void open_stores(MDBX_txn* txn) {
            m_meta.open(txn);
            m_change_log.open(txn);
            if (m_node_id == make_zero_node()) {
                NodeId node_id = m_meta.get_node_id(txn);
                if (compare_node_id(node_id, make_zero_node()) == 0) {
                    /// \note Node id generation is the responsibility of the
                    ///       sync engine. The accumulator simply ensures a
                    ///       stable per-thread node id is recorded. Until the
                    ///       sync engine lands in a follow-up PR, the first
                    ///       flush records an all-zero placeholder which the
                    ///       SyncEngine must overwrite on attach.
                    node_id = make_zero_node();
                }
                m_node_id = node_id;
            }
        }

        std::shared_ptr<Connection> m_conn;
        NodeId m_node_id{};
        MetaStore m_meta;
        ChangeLogStore m_change_log;
        std::mutex m_mutex;
        std::unordered_map<std::thread::id, std::vector<PendingOp>> m_pending;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_CHANGE_ACCUMULATOR_HPP_INCLUDED