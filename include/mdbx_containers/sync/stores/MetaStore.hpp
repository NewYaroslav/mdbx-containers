#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_STORES_META_STORE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_STORES_META_STORE_HPP_INCLUDED

/// \file MetaStore.hpp
/// \brief Persistent metadata for the sync subsystem: db identity, local node
/// identity, schema version, monotonic local seq, creation timestamp.

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <mdbx.h>

#include "../../detail/utils.hpp"
#include "../Common.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Stable schema version recorded in \c _mdbxc_meta.
    /// \details Bump this when \c ChangeBatch layout or \c ChangeOp semantics
    /// change in a way that requires a fresh database or migration.
    inline std::uint32_t meta_schema_version() { return 1; }

    /// \brief Thin wrapper around the \c _mdbxc_meta named MDBX sub-DBI.
    /// \details All keys are fixed-width little-endian; values are flat
    /// byte buffers. Designed to be opened once per environment and reused.
    class MetaStore {
    public:
        /// \brief Constructs a wrapper bound to \p env.
        /// \param env Live MDBX environment.
        /// \param dbi_name DBI name; defaults to \c _mdbxc_meta.
        MetaStore(MDBX_env* env, const std::string& dbi_name = "_mdbxc_meta")
            : m_env(env), m_dbi_name(dbi_name), m_dbi(0), m_open(false) {}

        /// \brief Opens the DBI inside the supplied write transaction.
        /// \details Idempotent. Must be called before any other access.
        void open(MDBX_txn* txn) {
            if (m_open) return;
            check_mdbx(
                mdbx_dbi_open(txn, m_dbi_name.c_str(), MDBX_CREATE, &m_dbi),
                "Failed to open MetaStore DBI"
            );
            m_open = true;
        }

        /// \brief Throws when the DBI has not been opened yet.
        void ensure_open() const {
            if (!m_open) {
                throw std::logic_error("MetaStore is not open");
            }
        }

        /// \brief Returns whether the DBI is open.
        bool is_open() const { return m_open; }

        /// \brief Returns the underlying MDBX DBI handle.
        MDBX_dbi handle() const { return m_dbi; }

        /// \brief Reads \c db_uuid. Empty array when unset.
        NodeId get_db_uuid(MDBX_txn* txn) const {
            NodeId out{};
            read_fixed(txn, key_db_uuid(), reinterpret_cast<std::uint8_t*>(out.data()), 16);
            return out;
        }

        /// \brief Writes \c db_uuid.
        void set_db_uuid(MDBX_txn* txn, const NodeId& value) {
            write_fixed(txn, key_db_uuid(), reinterpret_cast<const std::uint8_t*>(value.data()), 16);
        }

        /// \brief Reads \c node_id. Empty array when unset.
        NodeId get_node_id(MDBX_txn* txn) const {
            NodeId out{};
            read_fixed(txn, key_node_id(), reinterpret_cast<std::uint8_t*>(out.data()), 16);
            return out;
        }

        /// \brief Writes \c node_id.
        void set_node_id(MDBX_txn* txn, const NodeId& value) {
            write_fixed(txn, key_node_id(), reinterpret_cast<const std::uint8_t*>(value.data()), 16);
        }

        /// \brief Reads the schema version. Returns 0 when unset.
        std::uint32_t get_schema_version(MDBX_txn* txn) const {
            std::uint8_t buf[4];
            const std::size_t n = read_fixed(txn, key_schema_version(), buf, 4);
            if (n != 4) return 0;
            return static_cast<std::uint32_t>(buf[0]) |
                   (static_cast<std::uint32_t>(buf[1]) << 8) |
                   (static_cast<std::uint32_t>(buf[2]) << 16) |
                   (static_cast<std::uint32_t>(buf[3]) << 24);
        }

        /// \brief Writes the schema version.
        void set_schema_version(MDBX_txn* txn, std::uint32_t value) {
            const std::uint8_t buf[4] = {
                static_cast<std::uint8_t>(value & 0xff),
                static_cast<std::uint8_t>((value >> 8) & 0xff),
                static_cast<std::uint8_t>((value >> 16) & 0xff),
                static_cast<std::uint8_t>((value >> 24) & 0xff)
            };
            write_fixed(txn, key_schema_version(), buf, 4);
        }

        /// \brief Reads \c local_seq. Returns 0 when unset.
        std::uint64_t get_local_seq(MDBX_txn* txn) const {
            std::uint8_t buf[8];
            const std::size_t n = read_fixed(txn, key_local_seq(), buf, 8);
            if (n != 8) return 0;
            std::uint64_t v = 0;
            for (int i = 0; i < 8; ++i) {
                v |= static_cast<std::uint64_t>(buf[i]) << (i * 8);
            }
            return v;
        }

        /// \brief Writes \c local_seq.
        void set_local_seq(MDBX_txn* txn, std::uint64_t value) {
            std::uint8_t buf[8];
            for (int i = 0; i < 8; ++i) {
                buf[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
            }
            write_fixed(txn, key_local_seq(), buf, 8);
        }

        /// \brief Atomically increments \c local_seq and returns the new value.
        std::uint64_t increment_local_seq(MDBX_txn* txn) {
            const std::uint64_t next = get_local_seq(txn) + 1;
            set_local_seq(txn, next);
            return next;
        }

        /// \brief Reads \c created_at_ms. Returns 0 when unset.
        std::uint64_t get_created_at_ms(MDBX_txn* txn) const {
            std::uint8_t buf[8];
            const std::size_t n = read_fixed(txn, key_created_at_ms(), buf, 8);
            if (n != 8) return 0;
            std::uint64_t v = 0;
            for (int i = 0; i < 8; ++i) {
                v |= static_cast<std::uint64_t>(buf[i]) << (i * 8);
            }
            return v;
        }

        /// \brief Writes \c created_at_ms.
        void set_created_at_ms(MDBX_txn* txn, std::uint64_t value) {
            std::uint8_t buf[8];
            for (int i = 0; i < 8; ++i) {
                buf[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
            }
            write_fixed(txn, key_created_at_ms(), buf, 8);
        }

    private:
        static std::uint8_t key_db_uuid()       { return 0x01; }
        static std::uint8_t key_node_id()      { return 0x02; }
        static std::uint8_t key_schema_version(){ return 0x03; }
        static std::uint8_t key_local_seq()    { return 0x04; }
        static std::uint8_t key_created_at_ms() { return 0x05; }

        std::size_t read_fixed(MDBX_txn* txn, std::uint8_t key,
                               std::uint8_t* dst, std::size_t n) const {
            ensure_open();
            MDBX_val k = { &key, 1 };
            MDBX_val v;
            const int rc = mdbx_get(txn, m_dbi, &k, &v);
            if (rc == MDBX_NOTFOUND) return 0;
            check_mdbx(rc, "MetaStore read failed");
            const std::size_t got = v.iov_len < n ? v.iov_len : n;
            std::memcpy(dst, v.iov_base, got);
            return got;
        }

        void write_fixed(MDBX_txn* txn, std::uint8_t key,
                         const std::uint8_t* src, std::size_t n) const {
            ensure_open();
            MDBX_val k = { &key, 1 };
            MDBX_val v = { const_cast<std::uint8_t*>(src), n };
            check_mdbx(
                mdbx_put(txn, m_dbi, &k, &v, MDBX_UPSERT),
                "MetaStore write failed"
            );
        }

        MDBX_env*     m_env;
        std::string   m_dbi_name;
        MDBX_dbi      m_dbi;
        bool          m_open;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_STORES_META_STORE_HPP_INCLUDED