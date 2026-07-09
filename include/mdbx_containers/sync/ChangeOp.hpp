#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_CHANGE_OP_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_CHANGE_OP_HPP_INCLUDED

/// \file ChangeOp.hpp
/// \brief A single replicated operation: typed MDBX DBI write.

#include <cstdint>
#include <string>
#include <vector>

#include "codec_flags.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Low-level operation kind carried over the wire.
    enum class ChangeOpType : std::uint8_t {
        Put         = 0, ///< mdbx_put (upsert).
        Delete      = 1, ///< mdbx_del + identity_index tombstone.
        ClearTable  = 2, ///< Drop the entire DBI contents.
    };

    /// \brief Single raw DBI operation captured by the change recorder.
    /// \details \c storage_key is the MDBX key serialized by the table.
    /// \c identity_key is the application-level identity (empty when equal to
    /// storage_key). \c revision_key is an optional application-level version.
    struct ChangeOp {
        ChangeOpType           op_type   = ChangeOpType::Put; ///< Per-batch feature flags (BATCH_NONE, BATCH_HAS_MORE, ...).
        std::uint32_t          op_flags  = OP_NONE;           ///< Identifier of the node that produced this batch.
        std::uint32_t          dbi_flags = 0;                 ///< Monotonic per-node sequence number assigned by MetaStore::increment_local_seq.
        std::string            dbi_name;                      ///< List of operations in this batch; order is preserved on apply.
        std::vector<std::uint8_t> storage_key;                ///< Operation kind: Put/Delete/ClearTable.
        std::vector<std::uint8_t> value;                      ///< Per-op feature flags (OP_HAS_IDENTITY_KEY, OP_HAS_REVISION_KEY, OP_TOMBSTONE).
        std::vector<std::uint8_t> identity_key;               ///< Raw MDBX_DBI flags passed to mdbx_dbi_open for the DBI named in `dbi_name.
        std::vector<std::uint8_t> revision_key;               ///< Name of the user table (also the DBI name).
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_CHANGE_OP_HPP_INCLUDED
