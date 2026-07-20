#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_STORES_IDENTITY_INDEX_STORE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_STORES_IDENTITY_INDEX_STORE_HPP_INCLUDED

/// \file IdentityIndexStore.hpp
/// \brief Per-record identity index used for dedup and conflict detection
/// when \c identity_key differs from \c storage_key.

namespace mdbxc {
namespace sync {

    /// \brief Bit flags for the \c IdentityIndexValue::flags field.
    enum IdentityIndexFlags : std::uint32_t {
        /// \brief Default state: a live identity record.
        IDENTITY_NONE      = 0,
        /// \brief Marker that the logical record was deleted; the row stays
        /// in the index so older incoming batches can still resolve it.
        IDENTITY_TOMBSTONE = 1u << 0,
    };

    /// \brief Value layout for an identity-index record.
    /// \details Mirrors the wire payload of \c ChangeOp plus enough metadata
    /// to resolve the (dbi_name, identity_key) entry back to a physical
    /// storage_key without re-reading the user table.
    struct IdentityIndexValue {
        std::vector<std::uint8_t> storage_key;
        NodeId origin_node_id{};
        std::uint64_t seq = 0;
        std::vector<std::uint8_t> revision_key;
        std::uint32_t flags = 0;
    };

    /// \brief Thin wrapper around \c _mdbxc_identity_index.
    /// \details Key = \c u32 dbi_name_len_le || dbi_name_bytes || identity_key_bytes.
    /// The length prefix is mandatory: without it, \c ("ab","c") and
    /// \c ("a","bc") would collide on the same MDBX record. Value =
    /// \c IdentityIndexValue, opaque structured payload with length-prefixed
    /// variable-size fields (\c storage_key, \c revision_key).
    class IdentityIndexStore {
    public:
        IdentityIndexStore(MDBX_env* env,
                           const std::string& dbi_name = "_mdbxc_identity_index")
            : m_env(env), m_dbi_name(dbi_name), m_dbi(0), m_open(false) {}

        void open(MDBX_txn* txn) {
            txn = checked_txn(txn, "IdentityIndexStore::open");
            if (m_open) return;
            check_mdbx(
                mdbx_dbi_open(txn, m_dbi_name.c_str(), MDBX_CREATE, &m_dbi),
                "Failed to open IdentityIndexStore DBI"
            );
            m_open = true;
        }

        bool is_open() const { return m_open; }
        MDBX_dbi handle() const { return m_dbi; }

        /// \brief Throws when the DBI has not been opened yet.
        void ensure_open() const {
            if (!m_open) {
                throw std::logic_error("IdentityIndexStore is not open");
            }
        }

        /// \brief Stores or replaces the identity record.
        void put(MDBX_txn* txn, const std::string& dbi_name,
                 const std::vector<std::uint8_t>& identity_key,
                 const IdentityIndexValue& value) {
            txn = checked_txn(txn, "IdentityIndexStore::put");
            ensure_open();
            std::vector<std::uint8_t> key_buf;
            encode_key(dbi_name, identity_key, key_buf);
            std::vector<std::uint8_t> val_buf;
            encode_value(value, val_buf);
            MDBX_val k = { key_buf.empty() ? nullptr : &key_buf[0], key_buf.size() };
            MDBX_val v = { val_buf.empty() ? nullptr : &val_buf[0], val_buf.size() };
            check_mdbx(
                mdbx_put(txn, m_dbi, &k, &v, MDBX_UPSERT),
                "IdentityIndexStore put failed"
            );
        }

        /// \brief Reads the identity record.
        /// \return true when present, false when absent.
        bool get(MDBX_txn* txn, const std::string& dbi_name,
                 const std::vector<std::uint8_t>& identity_key,
                 IdentityIndexValue& out) const {
            txn = checked_txn(txn, "IdentityIndexStore::get");
            ensure_open();
            std::vector<std::uint8_t> key_buf;
            encode_key(dbi_name, identity_key, key_buf);
            MDBX_val k = { key_buf.empty() ? nullptr : &key_buf[0], key_buf.size() };
            MDBX_val v;
            const int rc = mdbx_get(txn, m_dbi, &k, &v);
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "IdentityIndexStore get failed");
            decode_value(v, out);
            return true;
        }

        /// \brief Marks the record with a tombstone. Does not delete.
        void tombstone(MDBX_txn* txn, const std::string& dbi_name,
                       const std::vector<std::uint8_t>& identity_key,
                       const IdentityIndexValue& marker) {
            txn = checked_txn(txn, "IdentityIndexStore::tombstone");
            ensure_open();
            IdentityIndexValue v = marker;
            v.flags |= static_cast<std::uint32_t>(IDENTITY_TOMBSTONE);
            put(txn, dbi_name, identity_key, v);
        }

        /// \brief Removes the identity record outright.
        /// \return true when a record was removed.
        bool erase(MDBX_txn* txn, const std::string& dbi_name,
                   const std::vector<std::uint8_t>& identity_key) {
            txn = checked_txn(txn, "IdentityIndexStore::erase");
            if (!m_open) {
                throw std::logic_error("IdentityIndexStore is not open");
            }
            std::vector<std::uint8_t> key_buf;
            encode_key(dbi_name, identity_key, key_buf);
            MDBX_val k = { key_buf.empty() ? nullptr : &key_buf[0], key_buf.size() };
            const int rc = mdbx_del(txn, m_dbi, &k, nullptr);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "IdentityIndexStore erase failed");
            return false;
        }

    private:
        MDBX_txn* checked_txn(MDBX_txn* txn, const char* context) const {
            return checked_txn_env(txn, m_env, context);
        }

        static void encode_key(const std::string& dbi_name,
                               const std::vector<std::uint8_t>& identity_key,
                               std::vector<std::uint8_t>& out) {
            const std::uint32_t dn_len =
                static_cast<std::uint32_t>(dbi_name.size());
            out.clear();
            out.reserve(4 + dn_len + identity_key.size());
            detail::append_u32_le(out, dn_len);
            out.insert(out.end(), dbi_name.begin(), dbi_name.end());
            out.insert(out.end(), identity_key.begin(), identity_key.end());
        }

        static void encode_value(const IdentityIndexValue& v,
                                 std::vector<std::uint8_t>& out) {
            out.clear();
            detail::append_u32_le(out, static_cast<std::uint32_t>(v.storage_key.size()));
            if (!v.storage_key.empty()) {
                out.insert(out.end(), v.storage_key.begin(), v.storage_key.end());
            }
            out.insert(out.end(), v.origin_node_id.begin(), v.origin_node_id.end());
            detail::append_u64_le(out, v.seq);
            detail::append_u32_le(out, static_cast<std::uint32_t>(v.revision_key.size()));
            if (!v.revision_key.empty()) {
                out.insert(out.end(), v.revision_key.begin(), v.revision_key.end());
            }
            detail::append_u32_le(out, v.flags);
        }

        static void decode_value(const MDBX_val& v, IdentityIndexValue& out) {
            const std::uint8_t* p = static_cast<const std::uint8_t*>(v.iov_base);
            std::size_t pos = 0;
            const std::size_t total = v.iov_len;
            auto need = [&pos, total](std::size_t n) -> void {
                if (pos + n > total) {
                    throw std::runtime_error("IdentityIndexStore decode underrun");
                }
            };
            need(4);
            std::uint32_t sk_len = detail::read_u32_le(p + pos);
            pos += 4;
            need(sk_len);
            out.storage_key.resize(sk_len);
            if (sk_len > 0) {
                std::memcpy(out.storage_key.data(), p + pos, sk_len);
            }
            pos += sk_len;
            need(16);
            std::memcpy(out.origin_node_id.data(), p + pos, 16);
            pos += 16;
            need(8);
            out.seq = detail::read_u64_le(p + pos);
            pos += 8;
            need(4);
            std::uint32_t rv_len = detail::read_u32_le(p + pos);
            pos += 4;
            need(rv_len);
            out.revision_key.resize(rv_len);
            if (rv_len > 0) {
                std::memcpy(out.revision_key.data(), p + pos, rv_len);
            }
            pos += rv_len;
            need(4);
            out.flags = detail::read_u32_le(p + pos);
        }

        MDBX_env*     m_env;
        std::string   m_dbi_name;
        MDBX_dbi      m_dbi;
        bool          m_open;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_STORES_IDENTITY_INDEX_STORE_HPP_INCLUDED
