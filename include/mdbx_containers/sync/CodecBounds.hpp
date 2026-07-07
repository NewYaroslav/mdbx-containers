#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_CODEC_BOUNDS_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_CODEC_BOUNDS_HPP_INCLUDED

/// \file CodecBounds.hpp
/// \brief Hard-coded codec bound defaults.

#include <cstdint>

namespace mdbxc {
namespace sync {

    /// \brief Default structural limits enforced by the codec decoder.
    /// \details Sync clients and servers should not exceed these values per
    /// batch. Larger values are rejected by \c ChangeBatchCodec::validate.
    struct CodecBounds {
        std::uint32_t max_ops_per_batch        = 10000;
        std::uint32_t max_dbi_name_len         = 256;
        std::uint32_t max_storage_key_len      = 16u * 1024u;
        std::uint32_t max_value_len            = 4u * 1024u * 1024u;
        std::uint32_t max_identity_key_len     = 16u * 1024u;
        std::uint32_t max_revision_key_len     = 16u * 1024u;
        std::uint32_t max_batch_total_bytes    = 64u * 1024u * 1024u;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_CODEC_BOUNDS_HPP_INCLUDED
