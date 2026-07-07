#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_CHANGE_BATCH_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_CHANGE_BATCH_HPP_INCLUDED

/// \file ChangeBatch.hpp
/// \brief A single atomic group of replicated \c ChangeOp operations.

#include <cstdint>
#include <vector>

#include "ChangeOp.hpp"
#include "CodecFlags.hpp"
#include "Common.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Versioned group of operations emitted by one origin node.
    /// \invariant All operations in a batch are recorded and applied under a
    /// single MDBX write transaction.
    struct ChangeBatch {
        std::uint32_t         version = 1;      ///< Schema version; always 1 in v0.1.
        std::uint32_t         batch_flags = BATCH_NONE;
        NodeId                origin_node_id{};
        std::uint64_t         seq = 0;
        std::uint64_t         time_unix_ns = 0; ///< Physical timestamp metadata only.
        std::vector<ChangeOp> ops;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_CHANGE_BATCH_HPP_INCLUDED
