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
///  - incompatible persistent DBI flags: \c Conflict.
///
/// Self-origin batches are always \c Skipped (the receiver already has them).
///
/// Stores (MetaStore, ChangeLogStore, AppliedStore) are opened inside the
/// supplied transaction and are therefore valid only for its lifetime.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
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
        /// name with the captured \c ChangeOp::dbi_flags plus
        /// \c MDBX_CREATE so destination tables keep compatible MDBX flags.
        ApplyResult apply_batch(MDBX_txn* txn, const ChangeBatch& batch) {
            MetaStore meta(m_conn->env_handle());
            AppliedStore applied(m_conn->env_handle());
            meta.open(txn);
            applied.open(txn);

            const NodeId local_node = meta.get_node_id(txn);
            if (compare_node_id(batch.origin_node_id, local_node) == 0) {
                return ApplyResult::Skipped;
            }

            const std::uint64_t last = applied.last_applied_seq(txn, batch.origin_node_id);
            if (batch.seq <= last) return ApplyResult::Skipped;
            if (batch.seq != last + 1) return ApplyResult::Conflict;
            if (!batch_has_consistent_dbi_flags(batch)) return ApplyResult::Conflict;

            std::unordered_map<std::string, MDBX_dbi> dbi_cache;
            if (!preflight_batch_user_dbis(txn, batch, dbi_cache)) {
                return ApplyResult::Conflict;
            }
            for (const ChangeOp& op : batch.ops) {
                apply_one_op(txn, op, dbi_cache);
            }
            applied.set_last_applied_seq(txn, batch.origin_node_id, batch.seq);
            return ApplyResult::Applied;
        }

        /// \brief Handles a pull request: scans the local \c ChangeLogStore
        /// for batches newer than the requester's cursor.
        /// \details When \c request.have is empty, returns a full snapshot
        /// (all known origins, batches from seq=1). Non-empty cursors still
        /// scan all origins so newly discovered origins and multi-origin
        /// pagination are not stranded. Validates \c request.db_id against
        /// the local \c db_uuid; mismatched peers receive an empty response
        /// with \c ok=false.
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
            /// RAII guard: aborts (releases handle + reader slot) instead of
            /// reset, because we never renew the same transaction here — reset
            /// would leak the MDBX_txn object across calls.
            struct Guard {
                MDBX_txn* t;
                ~Guard() { if (t) mdbx_txn_abort(t); }
            } guard{txn};

            MDBX_dbi changelog_dbi = open_changelog_ro(txn);
            if (changelog_dbi == 0) {
                out.remote_have = read_applied_cursor(txn, out.remote_have);
                return out;
            }

            return pull_full_snapshot(txn, changelog_dbi, request);
        }

        /// \brief Range-scans the changelog and returns batches newer than
        /// \c request.have.
        /// \details Empty \c request.have returns a full snapshot. Non-empty
        /// cursors filter each origin independently and still include origins
        /// missing from the cursor. Uses changelog keys to seek to
        /// \c have_seq+1 for each origin so old values are not decoded.
        /// Sets \c has_more=true when the walk stopped because of
        /// \c request.max_batches or \c request.max_bytes.
        PullResponse pull_full_snapshot(MDBX_txn* txn, MDBX_dbi dbi,
                                        const PullRequest& request) {
            PullResponse out;
            out.remote_have = read_applied_cursor(txn, out.remote_have);
            const std::vector<NodeId> origins = collect_changelog_origins(txn, dbi);
            std::size_t total_bytes = 0;
            bool truncated = false;
            for (std::size_t i = 0; i < origins.size(); ++i) {
                if (pull_origin_batches(txn, dbi, origins[i], request, out, total_bytes)) {
                    truncated = true;
                    break;
                }
            }
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

        /// \brief Builds a \c PushRequest that carries every local batch with
        /// \c seq in \c [from_seq, to_seq] (inclusive).
        /// \details Hides the system stores from callers: example code and
        /// future transports can call this instead of touching
        /// \c MetaStore / \c ChangeLogStore directly. The sender is always
        /// the local node id (derived from \c _mdbxc_meta); sending batches
        /// of other origins is not supported at this layer.
        ///
        /// Opens its own short-lived read-only transaction on the bound
        /// connection. The caller must not have another active transaction
        /// for the same connection on the current thread (Mdbx would return
        /// \c MDBX_BUSY). Right after a writable commit is fine.
        /// \param from_seq First \c seq to include (use 1 to send from the
        /// beginning; use the peer's \c applied_cursor + 1 to send a delta).
        /// \param to_seq Last \c seq to include (use 0 to send up to the
        /// current local tail, inclusive).
        /// \return A \c PushRequest ready to send to the peer. Empty
        /// \c batches when the requested range is empty (or past the tail).
        /// \throws std::runtime_error if the requested range is not
        /// contiguous in the local changelog (a hole means pruning,
        /// corruption, or a wrong origin).
        /// \throws MdbxException on database error.
        PushRequest make_push_request(std::uint64_t from_seq,
                                      std::uint64_t to_seq) const {
            PushRequest req;
            req.db_id  = db_uuid();
            if (to_seq != 0 && to_seq < from_seq) return req;
            auto txn = m_conn->transaction(TransactionMode::READ_ONLY);
            MetaStore meta(m_conn->env_handle());
            meta.open(txn.handle());
            ChangeLogStore log(m_conn->env_handle());
            log.open(txn.handle());
            req.sender = meta.get_node_id(txn.handle());
            if (to_seq == 0) {
                to_seq = meta.get_local_seq(txn.handle());
            }
            if (to_seq < from_seq) return req;
            std::vector<std::uint8_t> buf;
            for (std::uint64_t s = from_seq;; ++s) {
                if (!log.get(txn.handle(), req.sender, s, buf)) {
                    throw std::runtime_error(
                        "SyncEngine::make_push_request: changelog gap at seq " +
                        std::to_string(s) +
                        " (pruning, corruption, or wrong origin)");
                }
                req.batches.push_back(ChangeBatchCodec::decode_exact(buf));
                if (s == to_seq) break;
            }
            return req;
        }

    private:
        struct CursorGuard {
            explicit CursorGuard(MDBX_cursor* cursor) : raw(cursor) {}
            ~CursorGuard() { if (raw) mdbx_cursor_close(raw); }
            CursorGuard(const CursorGuard&) = delete;
            CursorGuard& operator=(const CursorGuard&) = delete;

            MDBX_cursor* raw;
        };

        /// \brief Returns true when \p request_db_id matches the local
        /// \c db_uuid. A zero \p request_db_id is rejected: callers must
        /// know the database identity before issuing pull/push requests.
        bool db_id_matches(const NodeId& request_db_id) const {
            const NodeId zero{};
            if (compare_node_id(request_db_id, zero) == 0) return false;
            return compare_node_id(request_db_id, db_uuid()) == 0;
        }

        static std::uint32_t persistent_dbi_flags_mask() {
            return static_cast<std::uint32_t>(MDBX_REVERSEKEY) |
                   static_cast<std::uint32_t>(MDBX_DUPSORT) |
                   static_cast<std::uint32_t>(MDBX_INTEGERKEY) |
                   static_cast<std::uint32_t>(MDBX_DUPFIXED) |
                   static_cast<std::uint32_t>(MDBX_INTEGERDUP) |
                   static_cast<std::uint32_t>(MDBX_REVERSEDUP);
        }

        static std::uint32_t persistent_dbi_flags(std::uint32_t dbi_flags) {
            return dbi_flags & persistent_dbi_flags_mask();
        }

        static bool batch_has_consistent_dbi_flags(const ChangeBatch& batch) {
            std::unordered_map<std::string, std::uint32_t> flags_by_name;
            for (const ChangeOp& op : batch.ops) {
                const std::uint32_t flags = persistent_dbi_flags(op.dbi_flags);
                const std::pair<std::unordered_map<std::string, std::uint32_t>::iterator, bool> inserted =
                    flags_by_name.insert(std::make_pair(op.dbi_name, flags));
                if (!inserted.second && inserted.first->second != flags) {
                    return false;
                }
            }
            return true;
        }

        static bool open_existing_user_dbi(MDBX_txn* txn,
                                           const std::string& name,
                                           std::uint32_t dbi_flags,
                                           MDBX_dbi& dbi) {
            const std::uint32_t open_dbi_flags = persistent_dbi_flags(dbi_flags);
            MDBX_db_flags_t open_flags = static_cast<MDBX_db_flags_t>(open_dbi_flags);
            int rc = mdbx_dbi_open(txn, name.c_str(), open_flags, &dbi);
            if (rc == MDBX_INCOMPATIBLE &&
                (open_dbi_flags & static_cast<std::uint32_t>(MDBX_INTEGERKEY)) == 0) {
                const MDBX_db_flags_t integer_flags = static_cast<MDBX_db_flags_t>(
                    open_dbi_flags | static_cast<std::uint32_t>(MDBX_INTEGERKEY));
                rc = mdbx_dbi_open(txn, name.c_str(), integer_flags, &dbi);
            }
            if (rc == MDBX_NOTFOUND) {
                dbi = 0;
                return true;
            }
            if (rc == MDBX_INCOMPATIBLE || rc == MDBX_EINVAL) {
                dbi = 0;
                return false;
            }
            check_mdbx(rc, "SyncEngine: failed to preflight user DBI '" + name + "'");
            return true;
        }

        static bool preflight_batch_user_dbis(MDBX_txn* txn,
                                              const ChangeBatch& batch,
                                              std::unordered_map<std::string, MDBX_dbi>& cache) {
            std::unordered_map<std::string, std::uint32_t> flags_by_name;
            for (const ChangeOp& op : batch.ops) {
                flags_by_name.insert(std::make_pair(op.dbi_name, persistent_dbi_flags(op.dbi_flags)));
            }

            for (std::unordered_map<std::string, std::uint32_t>::const_iterator it =
                     flags_by_name.begin();
                 it != flags_by_name.end(); ++it) {
                MDBX_dbi dbi = 0;
                if (!open_existing_user_dbi(txn, it->first, it->second, dbi)) {
                    return false;
                }
                if (dbi != 0) {
                    cache[it->first] = dbi;
                }
            }
            return true;
        }

        static std::vector<std::uint8_t> make_changelog_key(const NodeId& origin,
                                                            std::uint64_t seq) {
            std::vector<std::uint8_t> out(24);
            std::memcpy(out.data(), origin.data(), 16);
            for (int i = 0; i < 8; ++i) {
                out[16 + i] = static_cast<std::uint8_t>((seq >> ((7 - i) * 8)) & 0xff);
            }
            return out;
        }

        static NodeId changelog_key_origin(const MDBX_val& key) {
            if (key.iov_len != 24) {
                throw std::runtime_error("SyncEngine: invalid changelog key size");
            }
            NodeId origin{};
            std::memcpy(origin.data(), key.iov_base, 16);
            return origin;
        }

        static std::uint64_t changelog_key_seq(const MDBX_val& key) {
            if (key.iov_len != 24) {
                throw std::runtime_error("SyncEngine: invalid changelog key size");
            }
            const std::uint8_t* bytes = static_cast<const std::uint8_t*>(key.iov_base);
            std::uint64_t seq = 0;
            for (int i = 0; i < 8; ++i) {
                seq = (seq << 8) | static_cast<std::uint64_t>(bytes[16 + i]);
            }
            return seq;
        }

        static bool changelog_key_matches_origin(const MDBX_val& key,
                                                 const NodeId& origin) {
            return compare_node_id(changelog_key_origin(key), origin) == 0;
        }

        static std::vector<NodeId> collect_changelog_origins(MDBX_txn* txn,
                                                             MDBX_dbi dbi) {
            std::vector<NodeId> origins;
            MDBX_cursor* raw = nullptr;
            check_mdbx(mdbx_cursor_open(txn, dbi, &raw),
                       "pull_full: origin cursor open failed");
            CursorGuard guard(raw);

            MDBX_val k, v;
            std::vector<std::uint8_t> owned_key;
            int rc = mdbx_cursor_get(raw, &k, &v, MDBX_FIRST);
            while (rc == MDBX_SUCCESS) {
                const NodeId origin = changelog_key_origin(k);
                origins.push_back(origin);

                std::vector<std::uint8_t> next_key_buf =
                    make_changelog_key(origin, std::numeric_limits<std::uint64_t>::max());
                MDBX_val next_key = { next_key_buf.empty() ? nullptr : &next_key_buf[0],
                                      next_key_buf.size() };
                rc = mdbx_cursor_get(raw, &next_key, &v, MDBX_SET_RANGE);
                if (rc == MDBX_SUCCESS &&
                    compare_node_id(changelog_key_origin(next_key), origin) == 0) {
                    rc = mdbx_cursor_get(raw, &k, &v, MDBX_NEXT);
                } else if (rc == MDBX_SUCCESS) {
                    owned_key.resize(next_key.iov_len);
                    if (next_key.iov_len > 0) {
                        std::memcpy(owned_key.data(), next_key.iov_base, next_key.iov_len);
                    }
                    k.iov_base = owned_key.empty() ? nullptr : &owned_key[0];
                    k.iov_len = owned_key.size();
                }
            }
            if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                check_mdbx(rc, "pull_full: origin cursor walk failed");
            }
            return origins;
        }

        static bool pull_origin_batches(MDBX_txn* txn,
                                        MDBX_dbi dbi,
                                        const NodeId& origin,
                                        const PullRequest& request,
                                        PullResponse& out,
                                        std::size_t& total_bytes) {
            const std::uint64_t have_seq = request.have.last_seq_for(origin);
            if (have_seq == std::numeric_limits<std::uint64_t>::max()) {
                return false;
            }

            MDBX_cursor* raw = nullptr;
            check_mdbx(mdbx_cursor_open(txn, dbi, &raw),
                       "pull_full: origin batch cursor open failed");
            CursorGuard guard(raw);

            std::vector<std::uint8_t> key_buf = make_changelog_key(origin, have_seq + 1);
            MDBX_val k = { key_buf.empty() ? nullptr : &key_buf[0], key_buf.size() };
            MDBX_val v;
            int rc = mdbx_cursor_get(raw, &k, &v, MDBX_SET_RANGE);
            while (rc == MDBX_SUCCESS && changelog_key_matches_origin(k, origin)) {
                if (out.batches.size() >= request.max_batches ||
                    total_bytes >= request.max_bytes) {
                    return true;
                }

                const std::uint64_t key_seq = changelog_key_seq(k);
                std::vector<std::uint8_t> buf(v.iov_len);
                if (v.iov_len > 0) {
                    std::memcpy(buf.data(), v.iov_base, v.iov_len);
                }
                const ChangeBatch batch = ChangeBatchCodec::decode_exact(buf);
                if (compare_node_id(batch.origin_node_id, origin) != 0 ||
                    batch.seq != key_seq) {
                    throw std::runtime_error("SyncEngine: changelog key/value mismatch");
                }

                out.batches.push_back(batch);
                total_bytes += v.iov_len;
                rc = mdbx_cursor_get(raw, &k, &v, MDBX_NEXT);
            }
            if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                check_mdbx(rc, "pull_full: origin batch cursor walk failed");
            }
            return false;
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

        static MDBX_dbi resolve_user_dbi(MDBX_txn* txn,
                                         const std::string& name,
                                         std::uint32_t dbi_flags,
                                         std::unordered_map<std::string, MDBX_dbi>& cache) {
            auto it = cache.find(name);
            if (it != cache.end() && it->second != 0) {
                return it->second;
            }
            MDBX_dbi dbi = 0;
            const std::uint32_t open_dbi_flags = persistent_dbi_flags(dbi_flags);
            const MDBX_db_flags_t open_flags = static_cast<MDBX_db_flags_t>(
                open_dbi_flags | static_cast<std::uint32_t>(MDBX_CREATE));
            int rc = mdbx_dbi_open(txn, name.c_str(), open_flags, &dbi);
            if (rc == MDBX_INCOMPATIBLE &&
                (open_dbi_flags & static_cast<std::uint32_t>(MDBX_INTEGERKEY)) == 0) {
                // Compatibility path for batches produced before dbi_flags
                // capture was implemented. Existing integer-key DBIs may
                // reject open_flags=MDBX_CREATE with MDBX_INCOMPATIBLE.
                const MDBX_db_flags_t integer_flags = static_cast<MDBX_db_flags_t>(
                    static_cast<std::uint32_t>(open_flags) |
                    static_cast<std::uint32_t>(MDBX_INTEGERKEY));
                rc = mdbx_dbi_open(txn, name.c_str(), integer_flags, &dbi);
            }
            check_mdbx(rc, "SyncEngine: failed to open user DBI '" + name + "'");
            cache[name] = dbi;
            return dbi;
        }

        static void apply_one_op(MDBX_txn* txn,
                                 const ChangeOp& op,
                                 std::unordered_map<std::string, MDBX_dbi>& cache) {
            MDBX_dbi dbi = resolve_user_dbi(txn, op.dbi_name, op.dbi_flags, cache);
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
