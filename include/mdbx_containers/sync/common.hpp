#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_COMMON_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_COMMON_HPP_INCLUDED

/// \file common.hpp
/// \brief Shared sync types: node identifiers, fixed-size byte arrays.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace mdbxc {
namespace sync {

    /// \brief 16-byte stable identifier (UUID-like).
    using NodeId = std::array<std::uint8_t, 16>;

    /// \brief 16-byte database identifier.
    using DbId = std::array<std::uint8_t, 16>;

    /// \brief Creates a \c NodeId filled with zeros.
    inline NodeId make_zero_node() {
        NodeId out{};
        return out;
    }

    /// \brief Creates a \c NodeId from raw bytes.
    /// \param bytes Source buffer with at least 16 bytes.
    inline NodeId make_node_id(const std::uint8_t* bytes) {
        NodeId out{};
        std::memcpy(out.data(), bytes, 16);
        return out;
    }

    /// \brief Lexicographic compare of two \c NodeId values.
    /// \return Negative if \p a < \p b, zero if equal, positive otherwise.
    inline int compare_node_id(const NodeId& a, const NodeId& b) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) {
                return a[i] < b[i] ? -1 : 1;
            }
        }
        return 0;
    }

    /// \brief Stable lexical hash of a string (FNV-1a 64).
    /// \param s Input string.
    /// \return 64-bit hash.
    inline std::uint64_t fnv1a_64(const std::string& s) {
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (std::size_t i = 0; i < s.size(); ++i) {
            h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(s[i]));
            h *= 0x100000001b3ULL;
        }
        return h;
    }

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_COMMON_HPP_INCLUDED
