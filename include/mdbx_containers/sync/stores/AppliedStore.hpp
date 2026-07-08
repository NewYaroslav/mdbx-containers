#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_STORES_APPLIED_STORE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_STORES_APPLIED_STORE_HPP_INCLUDED

/// \file AppliedStore.hpp
/// \brief Tracks the last contiguous applied \c seq per origin \c NodeId.

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <mdbx.h>

#include "../../detail/utils.hpp"
#include "../Common.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Records the last contiguous applied \c seq per origin.
    /// \details Key = 16 bytes of \c NodeId. Value = 8 bytes little-endian
    /// \c seq. The store assumes the caller only writes contiguous updates
    /// (i.e. applying batch \c seq means \c last_seq = \c seq, never gaps).
    class AppliedStore {
    public:
        AppliedStore(MDBX_env* env,
                     const std::string& dbi_name = "_mdbxc_applied")
            : m_env(env), m_dbi_name(dbi_name), m_dbi(0), m_open(false) {}

        void open(MDBX_txn* txn) {
            if (m_open) return;
            check_mdbx(
                mdbx_dbi_open(txn, m_dbi_name.c_str(), MDBX_CREATE, &m_dbi),
                "Failed to open AppliedStore DBI"
            );
            m_open = true;
        }

        bool is_open() const { return m_open; }
        MDBX_dbi handle() const { return m_dbi; }

        /// \brief Throws when the DBI has not been opened yet.
        void ensure_open() const {
            if (!m_open) {
                throw std::logic_error("AppliedStore is not open");
            }
        }

        /// \brief Returns the last contiguous applied \c seq for \p origin.
        /// \details Returns 0 when no record exists.
        std::uint64_t last_applied_seq(MDBX_txn* txn, const NodeId& origin) const {
            ensure_open();
            MDBX_val k = { const_cast<std::uint8_t*>(origin.data()), 16 };
            MDBX_val v;
            const int rc = mdbx_get(txn, m_dbi, &k, &v);
            if (rc == MDBX_NOTFOUND) return 0;
            check_mdbx(rc, "AppliedStore read failed");
            if (v.iov_len != 8) return 0;
            std::uint64_t out = 0;
            for (int i = 0; i < 8; ++i) {
                out |= static_cast<std::uint64_t>(static_cast<std::uint8_t*>(v.iov_base)[i]) << (i * 8);
            }
            return out;
        }

        /// \brief Records \p seq as the new last contiguous applied value for
        /// \p origin.
        void set_last_applied_seq(MDBX_txn* txn, const NodeId& origin,
                                  std::uint64_t seq) {
            ensure_open();
            std::uint8_t buf[8];
            for (int i = 0; i < 8; ++i) {
                buf[i] = static_cast<std::uint8_t>((seq >> (i * 8)) & 0xff);
            }
            MDBX_val k = { const_cast<std::uint8_t*>(origin.data()), 16 };
            MDBX_val v = { buf, 8 };
            check_mdbx(
                mdbx_put(txn, m_dbi, &k, &v, MDBX_UPSERT),
                "AppliedStore write failed"
            );
        }

        /// \brief Removes the record for \p origin if present.
        /// \return true when a record was removed.
        bool clear(MDBX_txn* txn, const NodeId& origin) {
            ensure_open();
            MDBX_val k = { const_cast<std::uint8_t*>(origin.data()), 16 };
            const int rc = mdbx_del(txn, m_dbi, &k, nullptr);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "AppliedStore clear failed");
            return false;
        }

    private:
        MDBX_env*     m_env;
        std::string   m_dbi_name;
        MDBX_dbi      m_dbi;
        bool          m_open;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_STORES_APPLIED_STORE_HPP_INCLUDED