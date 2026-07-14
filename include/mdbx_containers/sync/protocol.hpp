#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_PROTOCOL_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_PROTOCOL_HPP_INCLUDED

/// \file protocol.hpp
/// \brief Transport-level request and response structures for sync.

#include <cstdint>
#include <string>
#include <vector>

#include "ChangeBatch.hpp"
#include "cancellation.hpp"
#include "common.hpp"
#include "SyncCursor.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Request from a replica to a primary node for new change batches.
    struct PullRequest {
        NodeId       requester{};
        DbId         db_id{};
        SyncCursor   have;
        std::uint64_t max_batches = 1000;
        std::uint64_t max_bytes   = 4ULL * 1024ULL * 1024ULL;
        /// \brief When true, the responder should produce a full snapshot
        /// instead of an incremental delta.
        bool         request_full_snapshot = false;
        /// \brief Cooperative cancellation token for this transport call.
        /// \details Optional; default-constructed tokens never cancel.
        /// Transports may poll it while waiting on interruptible operations.
        CancellationToken cancel_token;
    };

    /// \brief Response to a \c PullRequest.
    struct PullResponse {
        SyncCursor               remote_have;
        std::vector<ChangeBatch> batches;
        bool                     has_more = false;
        bool                     ok       = true;
        std::string              error;
    };

    /// \brief Request carrying changes that the sender wants the receiver to
    /// apply.
    struct PushRequest {
        NodeId                   sender{};
        DbId                     db_id{};
        std::vector<ChangeBatch> batches;
        /// \brief Cooperative cancellation token for this transport call.
        /// \details Optional; default-constructed tokens never cancel.
        CancellationToken        cancel_token;
    };

    /// \brief Response to a \c PushRequest.
    struct PushResponse {
        SyncCursor               receiver_have;
        bool                     ok = true;
        std::string              error;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_PROTOCOL_HPP_INCLUDED
