#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_STORES_CHANGE_LOG_STORE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_STORES_CHANGE_LOG_STORE_HPP_INCLUDED

/// \file ChangeLogStore.hpp
/// \brief Per-origin changelog of raw \c ChangeBatch bytes.

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <mdbx.h>

namespace mdbxc {
namespace sync {

    /// \brief Raw bytestore for replicated \c ChangeBatch records keyed by
    /// \c (origin_node_id, seq).
    /// \details Values are \c ChangeBatchCodec::encode() output, opaque to
    /// this layer. The store makes no assumption about seq monotonicity; the
    /// caller (SyncEngine) is responsible for gap detection and ordering.
    class ChangeLogStore {
    public:
        /// \brief Constructs a wrapper bound to \p env.
        ChangeLogStore(MDBX_env* env,
                       const std::string& dbi_name = "_mdbxc_changelog")
            : m_env(env), m_dbi_name(dbi_name), m_dbi(0), m_open(false) {}

        /// \brief Opens the DBI inside the supplied write transaction.
        void open(MDBX_txn* txn) {
            if (m_open) return;
            check_mdbx(
                mdbx_dbi_open(txn, m_dbi_name.c_str(), MDBX_CREATE, &m_dbi),
                "Failed to open ChangeLogStore DBI"
            );
            m_open = true;
        }

        bool is_open() const { return m_open; }
        MDBX_dbi handle() const { return m_dbi; }

        /// \brief Resets the open flag so the next \c open() reopens the DBI
        /// inside the supplied transaction. Use between transactions to avoid
        /// using a stale handle from a prior committed/closed transaction.
        void reset_open() { m_open = false; }

        /// \brief Throws when the DBI has not been opened yet.
        void ensure_open() const {
            if (!m_open) {
                throw std::logic_error("ChangeLogStore is not open");
            }
        }

        /// \brief Appends a single batch for \p origin at \p seq.
        /// \details Uses \c MDBX_NOOVERWRITE so accidental re-use of a
        /// (origin, seq) key surfaces immediately.
        void append(MDBX_txn* txn, const NodeId& origin,
                    std::uint64_t seq, const std::vector<std::uint8_t>& bytes) {
            ensure_open();
            std::vector<std::uint8_t> key_buf;
            encode_key(origin, seq, key_buf);
            MDBX_val k = { key_buf.empty() ? nullptr : &key_buf[0], key_buf.size() };
            MDBX_val v = { bytes.empty() ? nullptr : const_cast<std::uint8_t*>(&bytes[0]),
                           bytes.size() };
            check_mdbx(
                mdbx_put(txn, m_dbi, &k, &v, MDBX_NOOVERWRITE),
                "ChangeLogStore append failed"
            );
        }

        /// \brief Returns true when a record exists for (\p origin, \p seq).
        bool contains(MDBX_txn* txn, const NodeId& origin, std::uint64_t seq) const {
            ensure_open();
            std::vector<std::uint8_t> key_buf;
            encode_key(origin, seq, key_buf);
            MDBX_val k = { key_buf.empty() ? nullptr : &key_buf[0], key_buf.size() };
            MDBX_val v;
            const int rc = mdbx_get(txn, m_dbi, &k, &v);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "ChangeLogStore contains failed");
            return false;
        }

        /// \brief Reads raw batch bytes for (\p origin, \p seq).
        /// \return true when present, false when absent.
        bool get(MDBX_txn* txn, const NodeId& origin, std::uint64_t seq,
                 std::vector<std::uint8_t>& out) const {
            ensure_open();
            std::vector<std::uint8_t> key_buf;
            encode_key(origin, seq, key_buf);
            MDBX_val k = { key_buf.empty() ? nullptr : &key_buf[0], key_buf.size() };
            MDBX_val v;
            const int rc = mdbx_get(txn, m_dbi, &k, &v);
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "ChangeLogStore get failed");
            out.resize(v.iov_len);
            if (v.iov_len > 0) {
                std::memcpy(out.data(), v.iov_base, v.iov_len);
            }
            return true;
        }

        /// \brief Removes the record at (\p origin, \p seq).
        /// \return true when a record was removed, false when absent.
        bool erase(MDBX_txn* txn, const NodeId& origin, std::uint64_t seq) {
            ensure_open();
            std::vector<std::uint8_t> key_buf;
            encode_key(origin, seq, key_buf);
            MDBX_val k = { key_buf.empty() ? nullptr : &key_buf[0], key_buf.size() };
            const int rc = mdbx_del(txn, m_dbi, &k, nullptr);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "ChangeLogStore erase failed");
            return false;
        }

        /// \brief Removes every record with seq <= \p up_to for \p origin.
        /// \return Number of records removed.
        std::size_t prune_up_to(MDBX_txn* txn, const NodeId& origin,
                                std::uint64_t up_to) {
            if (!m_open) {
                throw std::logic_error("ChangeLogStore is not open");
            }
            MDBX_cursor* raw = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &raw), "cursor open failed");
            std::size_t removed = 0;
            try {
                std::vector<std::uint8_t> lo_key, hi_key;
                encode_key(origin, 0, lo_key);
                encode_key(origin, up_to, hi_key);
                MDBX_val lo = { lo_key.empty() ? nullptr : &lo_key[0], lo_key.size() };
                MDBX_val hi = { hi_key.empty() ? nullptr : &hi_key[0], hi_key.size() };
                MDBX_val k = lo;
                MDBX_val v;
                int rc = mdbx_cursor_get(raw, &k, &v, MDBX_SET_RANGE);
                while (rc == MDBX_SUCCESS) {
                    if (k.iov_len < 24) break;
                    if (mdbx_cmp(txn, m_dbi, &k, &hi) > 0) break;
                    rc = mdbx_cursor_del(raw, MDBX_CURRENT);
                    if (rc == MDBX_SUCCESS) {
                        ++removed;
                        rc = mdbx_cursor_get(raw, &k, &v, MDBX_NEXT);
                    } else if (rc == MDBX_NOTFOUND) {
                        break;
                    } else {
                        check_mdbx(rc, "ChangeLogStore prune cursor_del failed");
                    }
                }
                if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "ChangeLogStore prune cursor_get failed");
                }
            } catch (...) {
                mdbx_cursor_close(raw);
                throw;
            }
            mdbx_cursor_close(raw);
            return removed;
        }

    private:
        static void encode_key(const NodeId& origin, std::uint64_t seq,
                               std::vector<std::uint8_t>& out) {
            out.resize(24);
            std::memcpy(out.data(), origin.data(), 16);
            /// \brief Big-endian seq so MDBX bytewise range scans preserve
            ///        numeric ordering. Little-endian would invert order
            ///        around byte boundaries (e.g. 256 < 1).
            for (int i = 0; i < 8; ++i) {
                out[16 + i] = static_cast<std::uint8_t>((seq >> ((7 - i) * 8)) & 0xff);
            }
        }

        MDBX_env*     m_env;
        std::string   m_dbi_name;
        MDBX_dbi      m_dbi;
        bool          m_open;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_STORES_CHANGE_LOG_STORE_HPP_INCLUDED