#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_STORES_ORIGIN_INDEX_STORE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_STORES_ORIGIN_INDEX_STORE_HPP_INCLUDED

/// \file OriginIndexStore.hpp
/// \brief Compact index of origins present in the local changelog.

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <mdbx.h>

#include "../common.hpp"
#include "../../detail/utils.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Thin wrapper around \c _mdbxc_origins.
    /// \details Key = \c origin_node_id (16 raw bytes), value = max known
    /// changelog seq for that origin as \c u64 little-endian. The value is
    /// diagnostic/maintenance metadata; pull pagination uses the changelog
    /// keys for exact batch iteration.
    class OriginIndexStore {
    public:
        /// \brief Constructs a wrapper bound to \p env.
        OriginIndexStore(MDBX_env* env,
                         const std::string& dbi_name = "_mdbxc_origins")
            : m_env(env), m_dbi_name(dbi_name), m_dbi(0), m_open(false) {}

        /// \brief Opens or creates the DBI inside the supplied transaction.
        void open(MDBX_txn* txn) {
            if (m_open) return;
            int rc = mdbx_dbi_open(txn, m_dbi_name.c_str(), MDBX_CREATE, &m_dbi);
            if (rc == MDBX_EACCESS) {
                rc = mdbx_dbi_open(txn, m_dbi_name.c_str(),
                                   static_cast<MDBX_db_flags_t>(0), &m_dbi);
            }
            check_mdbx(rc, "Failed to open OriginIndexStore DBI");
            m_open = true;
        }

        /// \brief Opens the DBI only if it already exists.
        /// \return \c true when opened, \c false when the DBI is absent.
        bool open_existing(MDBX_txn* txn) {
            if (m_open) return true;
            const int rc = mdbx_dbi_open(txn, m_dbi_name.c_str(),
                                         static_cast<MDBX_db_flags_t>(0), &m_dbi);
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to open existing OriginIndexStore DBI");
            m_open = true;
            return true;
        }

        bool is_open() const { return m_open; }
        MDBX_dbi handle() const { return m_dbi; }

        /// \brief Resets the open flag so the next \c open() reopens the DBI.
        void reset_open() { m_open = false; }

        /// \brief Throws when the DBI has not been opened yet.
        void ensure_open() const {
            if (!m_open) {
                throw std::logic_error("OriginIndexStore is not open");
            }
        }

        /// \brief Returns true when the index contains no origins.
        bool empty(MDBX_txn* txn) const {
            ensure_open();
            MDBX_cursor* raw = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &raw),
                       "OriginIndexStore empty cursor open failed");
            MDBX_val k, v;
            const int rc = mdbx_cursor_get(raw, &k, &v, MDBX_FIRST);
            mdbx_cursor_close(raw);
            if (rc == MDBX_NOTFOUND) return true;
            check_mdbx(rc, "OriginIndexStore empty cursor get failed");
            return false;
        }

        /// \brief Records \p origin with at least \p seq as its known tail.
        void note_origin(MDBX_txn* txn, const NodeId& origin, std::uint64_t seq) {
            ensure_open();
            std::uint64_t current = 0;
            if (last_seq(txn, origin, current) && current >= seq) {
                return;
            }
            std::uint8_t value[8];
            encode_u64_le(seq, value);
            MDBX_val k = { const_cast<std::uint8_t*>(origin.data()), origin.size() };
            MDBX_val v = { value, sizeof(value) };
            check_mdbx(mdbx_put(txn, m_dbi, &k, &v, MDBX_UPSERT),
                       "OriginIndexStore note_origin failed");
        }

        /// \brief Reads the max known changelog seq for \p origin.
        /// \return true when the origin exists.
        bool last_seq(MDBX_txn* txn, const NodeId& origin,
                      std::uint64_t& out) const {
            ensure_open();
            MDBX_val k = { const_cast<std::uint8_t*>(origin.data()), origin.size() };
            MDBX_val v;
            const int rc = mdbx_get(txn, m_dbi, &k, &v);
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "OriginIndexStore last_seq failed");
            if (v.iov_len != 8) {
                throw std::runtime_error("OriginIndexStore value has invalid size");
            }
            out = decode_u64_le(v);
            return true;
        }

        /// \brief Returns all indexed origins in DB key order.
        std::vector<NodeId> origins(MDBX_txn* txn) const {
            ensure_open();
            std::vector<NodeId> out;
            MDBX_cursor* raw = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &raw),
                       "OriginIndexStore origins cursor open failed");
            try {
                MDBX_val k, v;
                int rc = mdbx_cursor_get(raw, &k, &v, MDBX_FIRST);
                while (rc == MDBX_SUCCESS) {
                    if (k.iov_len != 16) {
                        throw std::runtime_error("OriginIndexStore key has invalid size");
                    }
                    NodeId origin{};
                    std::memcpy(origin.data(), k.iov_base, 16);
                    out.push_back(origin);
                    rc = mdbx_cursor_get(raw, &k, &v, MDBX_NEXT);
                }
                if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "OriginIndexStore origins cursor walk failed");
                }
            } catch (...) {
                mdbx_cursor_close(raw);
                throw;
            }
            mdbx_cursor_close(raw);
            return out;
        }

    private:
        static void encode_u64_le(std::uint64_t value, std::uint8_t out[8]) {
            for (int i = 0; i < 8; ++i) {
                out[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
            }
        }

        static std::uint64_t decode_u64_le(const MDBX_val& value) {
            const std::uint8_t* bytes =
                static_cast<const std::uint8_t*>(value.iov_base);
            std::uint64_t out = 0;
            for (int i = 0; i < 8; ++i) {
                out |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
            }
            return out;
        }

        MDBX_env*     m_env;
        std::string   m_dbi_name;
        MDBX_dbi      m_dbi;
        bool          m_open;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_STORES_ORIGIN_INDEX_STORE_HPP_INCLUDED
