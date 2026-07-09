#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_SYNC_ENGINE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_SYNC_ENGINE_HPP_INCLUDED

/// \file SyncEngine.hpp
/// \brief Pull/push/apply coordinator for replication.
///
/// Lifecycle: call \c initialize_local_identity once per database, then drive
/// replication through \c handle_pull / \c handle_push / \c apply_batch.
/// \c handle_pull and \c handle_push open their own transactions on the
/// engine's \c Connection; \c apply_batch uses a caller-supplied write
/// transaction so multiple batches can be applied atomically.
///
/// Apply rules (v0.1):
///  - \c seq <= last_applied_seq: \c Skipped (redundant replay).
///  - \c seq == last_applied_seq + 1: \c Applied, ops applied in order.
///  - \c seq >  last_applied_seq + 1: \c Conflict (gap; caller must re-pull).
///
/// Self-origin batches are always \c Skipped (the receiver already has them).
///
/// Stores (MetaStore, ChangeLogStore, AppliedStore) are opened inside the
/// supplied transaction and are therefore valid only for its lifetime.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <mdbx.h>

#include "common.hpp"
#include "ConflictPolicy.hpp"
#include "ChangeBatch.hpp"
#include "ChangeBatchCodec.hpp"
#include "ChangeOp.hpp"
#include "protocol.hpp"
#include "SyncCursor.hpp"
#include "../common/MdbxException.hpp"
#include "../common/Transaction.hpp"
#include "../detail/utils.hpp"
#include "stores/AppliedStore.hpp"
#include "stores/ChangeLogStore.hpp"
#include "stores/MetaStore.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Outcome of a single \c apply_batch call.
    enum class ApplyResult {
        Applied,  ///< Batch was applied to local DBIs.
        Skipped,  ///< Batch was redundant (seq <= last contiguous applied)
                  ///< or originated from the local node.
        Conflict, ///< Gap detected (seq > last + 1); not applied.
    };

    /// \brief Pull/push/apply coordinator bound to a single \c Connection.
    class SyncEngine {
    public:
        /// \brief Constructs an engine bound to \p conn.
        /// \param conn Shared connection that owns the env and stores.
        /// \param policy Conflict resolution policy (default: \c Reject).
        explicit SyncEngine(std::shared_ptr<Connection> conn,
                            ConflictPolicy policy = ConflictPolicy::Reject)
            : m_conn(std::move(conn)), m_policy(policy) {}

        /// \brief Initialises the local \c node_id and \c db_uuid.
        /// \details Throws when already initialised with different values.
        /// \param node_id Stable 16-byte identifier for this node.
        /// \param db_uuid Stable 16-byte identifier for the replicated database.
        void initialize_local_identity(const NodeId& node_id,
                                      const NodeId& db_uuid) {
            auto txn = m_conn->transaction(TransactionMode::WRITABLE);
            MetaStore meta(m_conn->env_handle());
            meta.open(txn.handle());
            const NodeId existing_node = meta.get_node_id(txn.handle());
            const NodeId zero{};
            if (compare_node_id(existing_node, zero) != 0 &&
                compare_node_id(existing_node, node_id) != 0) {
                throw std::logic_error(
                    "SyncEngine already initialised with a different node_id");
            }
            const NodeId existing_db = meta.get_db_uuid(txn.handle());
            if (compare_node_id(existing_db, zero) != 0 &&
                compare_node_id(existing_db, db_uuid) != 0) {
                throw std::logic_error(
                    "SyncEngine already initialised with a different db_uuid");
            }
            meta.set_node_id(txn.handle(), node_id);
            meta.set_db_uuid(txn.handle(), db_uuid);
            meta.set_schema_version(txn.handle(), meta_schema_version());
            txn.commit();
        }

        /// \brief Returns the local \c node_id.
        NodeId local_node_id() const {
            auto txn = m_conn->transaction(TransactionMode::READ_ONLY);
            MetaStore meta(m_conn->env_handle());
            meta.open(txn.handle());
            return meta.get_node_id(txn.handle());
        }

        /// \brief Returns the database \c db_uuid.
        NodeId db_uuid() const {
            auto txn = m_conn->transaction(TransactionMode::READ_ONLY);
            MetaStore meta(m_conn->env_handle());
            meta.open(txn.handle());
            return meta.get_db_uuid(txn.handle());
        }

        /// \brief Returns the conflict resolution policy.
        ConflictPolicy policy() const noexcept { return m_policy; }

        /// \brief Applies a single \c ChangeBatch to local DBIs inside \p txn.
        /// \details See class-level docs for the seq / apply rules. The
        /// caller commits the transaction. User DBIs are opened lazily by
        /// name with \c MDBX_CREATE; existing DBIs are reused within \p txn.
        ApplyResult apply_batch(MDBX_txn* txn, const ChangeBatch& batch) {
            MetaStore meta(m_conn->env_handle());
            ChangeLogStore changelog(m_conn->env_handle());
            AppliedStore applied(m_conn->env_handle());
            meta.open(txn);
            changelog.open(txn);
            applied.open(txn);

            const NodeId local_node = meta.get_node_id(txn);
            if (compare_node_id(batch.origin_node_id, local_node) == 0) {
                return ApplyResult::Skipped;
            }

            const std::uint64_t last = applied.last_applied_seq(txn, batch.origin_node_id);
            if (batch.seq <= last) return ApplyResult::Skipped;
            if (batch.seq != last + 1) return ApplyResult::Conflict;

            std::unordered_map<std::string, MDBX_dbi> dbi_cache;
            for (const ChangeOp& op : batch.ops) {
                apply_one_op(txn, op, dbi_cache);
            }
            applied.set_last_applied_seq(txn, batch.origin_node_id, batch.seq);
            return ApplyResult::Applied;
        }

                /// \brief Handles a pull request: scans the local \c ChangeLogStore
        /// for batches newer than the requester's cursor.
        /// \details When \c request.have is empty, returns a full snapshot
        /// (all known origins, batches from seq=1). Validates
        /// \c request.db_id against the local \c db_uuid; mismatched peers
        /// receive an empty response with \c ok=false.
        /// \c has_more is set to \c true when the loop stopped because of
        /// \c request.max_batches or \c request.max_bytes rather than
        /// running out of changelog entries.
        PullResponse handle_pull(const PullRequest& request) {
            PullResponse out;
            if (!db_id_matches(request.db_id)) {
                out.ok = false;
                out.error = "db_id mismatch";
                return out;
            }

            MDBX_txn* txn = nullptr;
            check_mdbx(mdbx_txn_begin(m_conn->env_handle(), nullptr,
                                      MDBX_TXN_RDONLY, &txn),
                       "SyncEngine: failed to begin read txn for pull");
            struct Guard {
                MDBX_txn* t;
                ~Guard() { if (t) mdbx_txn_reset(t); }
            } guard{txn};

            MDBX_dbi changelog_dbi = open_changelog_ro(txn);
            if (changelog_dbi == 0) {
                out.remote_have = read_applied_cursor(txn, out.remote_have);
                return out;
            }

            if (request.have.last_seq_by_origin.empty()) {
                return pull_full_snapshot(txn, changelog_dbi, request);
            }

            std::vector<std::uint8_t> buf;
            std::size_t total_bytes = 0;
            bool truncated = false;
            for (const auto& kv : request.have.last_seq_by_origin) {
                const NodeId& origin = kv.first;
                const std::uint64_t have_seq = kv.second;
                std::uint64_t next_seq = have_seq + 1;
                while (out.batches.size() < request.max_batches &&
                       total_bytes < request.max_bytes) {
                    if (!changelog_get_ro(txn, changelog_dbi, origin,
                                          next_seq, buf)) {
                        break;
                    }
                    const ChangeBatch b = ChangeBatchCodec::decode_exact(buf);
                    out.batches.push_back(b);
                    total_bytes += buf.size();
                    ++next_seq;
                }
                if (out.batches.size() >= request.max_batches ||
                    total_bytes >= request.max_bytes) {
                    truncated = true;
                    break;
                }
            }
            out.has_more = truncated;
            out.remote_have = read_applied_cursor(txn, out.remote_have);
            return out;
        }

        /// \brief Walks the changelog with a cursor and returns every batch.
        /// \details Sets \c has_more=true when the walk stopped because of
        /// \c request.max_batches or \c request.max_bytes.
        PullResponse pull_full_snapshot(MDBX_txn* txn, MDBX_dbi dbi,
                                        const PullRequest& request) {
            PullResponse out;
            out.remote_have = read_applied_cursor(txn, out.remote_have);
            MDBX_cursor* raw = nullptr;
            check_mdbx(mdbx_cursor_open(txn, dbi, &raw), "pull_full: cursor open failed");
            std::size_t total_bytes = 0;
            bool truncated = false;
            try {
                MDBX_val k, v;
                int rc = mdbx_cursor_get(raw, &k, &v, MDBX_FIRST);
                while (rc == MDBX_SUCCESS) {
                    if (out.batches.size() >= request.max_batches ||
                        total_bytes >= request.max_bytes) {
                        truncated = true;
                        break;
                    }
                    std::vector<std::uint8_t> buf(v.iov_len);
                    if (v.iov_len > 0) {
                        std::memcpy(buf.data(), v.iov_base, v.iov_len);
                    }
                    out.batches.push_back(ChangeBatchCodec::decode_exact(buf));
                    total_bytes += v.iov_len;
                    rc = mdbx_cursor_get(raw, &k, &v, MDBX_NEXT);
                }
                if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "pull_full: cursor walk failed");
                }
            } catch (...) {
                mdbx_cursor_close(raw);
                throw;
            }
            mdbx_cursor_close(raw);
            out.has_more = truncated;
            return out;
        }

        /// \brief Handles a push request: applies each batch in order.
        /// \details Atomic: when \c apply_batch returns \c Conflict for any
        /// batch, the transaction is rolled back (no partial commit) and
        /// \c ok is set to \c false. Validates \c request.db_id against the
        /// local \c db_uuid; mismatched peers receive \c ok=false with no
        /// side effects.
        PushResponse handle_push(const PushRequest& request) {
            PushResponse out;
            if (!db_id_matches(request.db_id)) {
                out.ok = false;
                out.error = "db_id mismatch";
                out.receiver_have = applied_cursor();
                return out;
            }
            auto txn = m_conn->transaction(TransactionMode::WRITABLE);
            for (const ChangeBatch& batch : request.batches) {
                const ApplyResult r = apply_batch(txn.handle(), batch);
                if (r == ApplyResult::Conflict) {
                    out.ok = false;
                    out.error = "gap/conflict while applying pushed batch";
                    return out;  // txn dtor rolls back; receiver_have stays stale
                }
            }
            txn.commit();
            out.ok = true;
            out.receiver_have = applied_cursor();
            return out;
        }

        /// \brief Returns the current applied cursor across all known origins.
        SyncCursor applied_cursor() const {
            SyncCursor cur;
            auto txn = m_conn->transaction(TransactionMode::READ_ONLY);
            return read_applied_cursor(txn.handle(), cur);
        }

        /// \brief Reads the applied cursor using the caller-supplied txn.
        /// \details Used inside \c handle_pull to avoid opening a second
        /// sticky-thread transaction on the same thread.
        SyncCursor applied_cursor(MDBX_txn* txn) const {
            SyncCursor cur;
            return read_applied_cursor(txn, cur);
        }

    private:
        /// \brief Returns true when \p request_db_id matches the local
        /// \c db_uuid. A zero \p request_db_id is rejected: callers must
        /// know the database identity before issuing pull/push requests.
        bool db_id_matches(const NodeId& request_db_id) const {
            const NodeId zero{};
            if (compare_node_id(request_db_id, zero) == 0) return false;
            return compare_node_id(request_db_id, db_uuid()) == 0;
        }

        /// \brief Opens \c _mdbxc_changelog read-only when it exists; 0 otherwise.
        static MDBX_dbi open_changelog_ro(MDBX_txn* txn) {
            return open_store_ro(txn, "_mdbxc_changelog");
        }

        /// \brief Opens \c _mdbxc_applied read-only when it exists; 0 otherwise.
        static MDBX_dbi open_applied_ro(MDBX_txn* txn) {
            return open_store_ro(txn, "_mdbxc_applied");
        }

        static MDBX_dbi open_store_ro(MDBX_txn* txn, const char* name) {
            MDBX_dbi dbi = 0;
            const int rc = mdbx_dbi_open(txn, name, static_cast<MDBX_db_flags_t>(0), &dbi);
            if (rc == MDBX_NOTFOUND) return 0;
            check_mdbx(rc, std::string("SyncEngine: failed to open store '") + name + "'");
            return dbi;
        }

        static SyncCursor read_applied_cursor(MDBX_txn* txn, SyncCursor cur) {
            MDBX_dbi applied_dbi = open_applied_ro(txn);
            if (applied_dbi == 0) return cur;
            MDBX_cursor* raw = nullptr;
            check_mdbx(mdbx_cursor_open(txn, applied_dbi, &raw),
                       "applied_cursor: cursor open failed");
            try {
                MDBX_val k, v;
                int rc = mdbx_cursor_get(raw, &k, &v, MDBX_FIRST);
                while (rc == MDBX_SUCCESS) {
                    if (k.iov_len == 16) {
                        NodeId origin{};
                        std::memcpy(origin.data(), k.iov_base, 16);
                        if (v.iov_len == 8) {
                            std::uint64_t seq = 0;
                            for (int i = 0; i < 8; ++i) {
                                seq |= static_cast<std::uint64_t>(
                                           static_cast<std::uint8_t*>(v.iov_base)[i])
                                       << (i * 8);
                            }
                            cur.last_seq_by_origin[origin] = seq;
                        }
                    }
                    rc = mdbx_cursor_get(raw, &k, &v, MDBX_NEXT);
                }
                if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "applied_cursor: cursor walk failed");
                }
            } catch (...) {
                mdbx_cursor_close(raw);
                throw;
            }
            mdbx_cursor_close(raw);
            return cur;
        }

        static bool changelog_get_ro(MDBX_txn* txn, MDBX_dbi dbi,
                                     const NodeId& origin, std::uint64_t seq,
                                     std::vector<std::uint8_t>& out) {
            std::vector<std::uint8_t> key_buf(24);
            std::memcpy(key_buf.data(), origin.data(), 16);
            for (int i = 0; i < 8; ++i) {
                key_buf[16 + i] = static_cast<std::uint8_t>((seq >> ((7 - i) * 8)) & 0xff);
            }
            MDBX_val k = { key_buf.empty() ? nullptr : &key_buf[0], key_buf.size() };
            MDBX_val v;
            const int rc = mdbx_get(txn, dbi, &k, &v);
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "changelog read failed");
            out.resize(v.iov_len);
            if (v.iov_len > 0) {
                std::memcpy(out.data(), v.iov_base, v.iov_len);
            }
            return true;
        }

        static MDBX_dbi resolve_user_dbi(MDBX_txn* txn,
                                         const std::string& name,
                                         std::unordered_map<std::string, MDBX_dbi>& cache) {
            auto it = cache.find(name);
            if (it != cache.end() && it->second != 0) {
                return it->second;
            }
            MDBX_dbi dbi = 0;
            check_mdbx(mdbx_dbi_open(txn, name.c_str(), MDBX_CREATE, &dbi),
                       "SyncEngine: failed to open user DBI '" + name + "'");
            cache[name] = dbi;
            return dbi;
        }

        static void apply_one_op(MDBX_txn* txn,
                                 const ChangeOp& op,
                                 std::unordered_map<std::string, MDBX_dbi>& cache) {
            MDBX_dbi dbi = resolve_user_dbi(txn, op.dbi_name, cache);
            switch (op.op_type) {
                case ChangeOpType::Put: {
                    MDBX_val k = { op.storage_key.empty() ? nullptr
                                                           : const_cast<std::uint8_t*>(op.storage_key.data()),
                                   op.storage_key.size() };
                    MDBX_val v = { op.value.empty() ? nullptr
                                                     : const_cast<std::uint8_t*>(op.value.data()),
                                   op.value.size() };
                    check_mdbx(mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT),
                               "SyncEngine: mdbx_put failed for DBI '" + op.dbi_name + "'");
                    return;
                }
                case ChangeOpType::Delete: {
                    MDBX_val k = { op.storage_key.empty() ? nullptr
                                                           : const_cast<std::uint8_t*>(op.storage_key.data()),
                                   op.storage_key.size() };
                    const int rc = mdbx_del(txn, dbi, &k, nullptr);
                    if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                        check_mdbx(rc, "SyncEngine: mdbx_del failed for DBI '" + op.dbi_name + "'");
                    }
                    cache.erase(op.dbi_name);
                    return;
                }
                case ChangeOpType::ClearTable: {
                    check_mdbx(mdbx_drop(txn, dbi, 0),
                               "SyncEngine: mdbx_drop failed for DBI '" + op.dbi_name + "'");
                    cache.erase(op.dbi_name);
                    return;
                }
            }
            throw std::logic_error("SyncEngine: unknown ChangeOpType");
        }

        std::shared_ptr<Connection> m_conn;
        ConflictPolicy              m_policy;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_SYNC_ENGINE_HPP_INCLUDED