#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_CONFLICT_POLICY_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_CONFLICT_POLICY_HPP_INCLUDED

/// \file ConflictPolicy.hpp
/// \brief How a replica resolves conflicting updates to the same logical key.

#include <cstdint>

namespace mdbxc {
namespace sync {

    /// \brief Conflict resolution policy applied during replication.
    enum class ConflictPolicy {
        /// \brief Reject the incoming change; surface an error to the caller.
        Reject,
        /// \brief Deterministically pick the change with the larger
        /// \c revision_key (lexicographic), tie-broken by \c origin_node_id.
        LastWriterWins,
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_CONFLICT_POLICY_HPP_INCLUDED
