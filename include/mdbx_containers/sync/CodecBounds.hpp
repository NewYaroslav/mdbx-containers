#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_CODEC_BOUNDS_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_CODEC_BOUNDS_HPP_INCLUDED

/// \file CodecBounds.hpp
/// \brief Hard-coded codec bound defaults.

namespace mdbxc {
namespace sync {

    /// \brief Default structural limits enforced by the codec decoder.
    /// \details Sync clients and servers should not exceed these values per
    /// batch. Larger values are rejected by \c ChangeBatchCodec::validate.
    struct CodecBounds {
        std::uint32_t max_ops_per_batch        = 10000;               ///< Per-batch feature flags (BATCH_NONE, BATCH_HAS_MORE, ...).
        std::uint32_t max_dbi_name_len         = 256;                 ///< Identifier of the node that produced this batch.
        std::uint32_t max_storage_key_len      = 16u * 1024u;         ///< Monotonic per-node sequence number assigned by MetaStore::increment_local_seq.
        std::uint32_t max_value_len            = 4u * 1024u * 1024u;  ///< List of operations in this batch; order is preserved on apply.
        std::uint32_t max_identity_key_len     = 16u * 1024u;         ///< Operation kind: Put/Delete/ClearTable.
        std::uint32_t max_revision_key_len     = 16u * 1024u;         ///< Per-op feature flags (OP_HAS_IDENTITY_KEY, OP_HAS_REVISION_KEY, OP_TOMBSTONE).
        std::uint32_t max_batch_total_bytes    = 64u * 1024u * 1024u; ///< Raw MDBX_DBI flags passed to mdbx_dbi_open for the DBI named in `dbi_name.
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_CODEC_BOUNDS_HPP_INCLUDED
