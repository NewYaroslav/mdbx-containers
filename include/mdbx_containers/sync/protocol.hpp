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

    /// \brief Machine-readable classification for sync-level response errors.
    /// \details This classifies errors produced by the sync protocol or
    /// engine itself. Transport-local failures such as HTTP status codes,
    /// WebSocket close codes, authentication rejection, and rate limits remain
    /// represented by transport retry hints and adapter metadata.
    enum class SyncResponseErrorCode : std::uint16_t {
        None                    = 0, ///< No structured sync error.
        DbIdMismatch            = 1, ///< Request targeted a different db_id.
        UnsupportedFullSnapshot = 2, ///< Full snapshot protocol is not implemented.
        ApplyConflict           = 3, ///< Push apply failed on a sync conflict.
        SnapshotRequired        = 4, ///< Requested changelog history was pruned.
    };

    /// \brief Returns a stable diagnostic name for a sync response error code.
    inline const char* sync_response_error_code_name(
            SyncResponseErrorCode code) {
        switch (code) {
            case SyncResponseErrorCode::None:
                return "none";
            case SyncResponseErrorCode::DbIdMismatch:
                return "db_id_mismatch";
            case SyncResponseErrorCode::UnsupportedFullSnapshot:
                return "unsupported_full_snapshot";
            case SyncResponseErrorCode::ApplyConflict:
                return "apply_conflict";
            case SyncResponseErrorCode::SnapshotRequired:
                return "snapshot_required";
        }
        return "unknown";
    }

    /// \brief Request from a replica to a primary node for new change batches.
    struct PullRequest {
        NodeId       requester{};
        DbId         db_id{};
        SyncCursor   have;
        std::uint64_t max_batches = 1000;
        std::uint64_t max_bytes   = 4ULL * 1024ULL * 1024ULL;
        /// \brief Requests a full snapshot instead of an incremental delta.
        /// \details Reserved for a future snapshot protocol. v0.1
        /// \c SyncEngine responders reject this request explicitly.
        bool         request_full_snapshot = false;
        /// \brief Cooperative cancellation token for this transport call.
        /// \details Optional; default-constructed tokens never cancel.
        /// Transports may poll it while waiting on interruptible operations.
        CancellationToken cancel_token;
    };

    /// \brief Response to a \c PullRequest.
    struct PullResponse {
        SyncCursor               remote_have;
        /// \brief Latest known changelog tail on the responder.
        /// \details Used for best-effort progress estimates. When
        /// \c remote_tail_known is false, the responder could not provide a
        /// reliable tail cursor for this page.
        SyncCursor               remote_tail;
        bool                     remote_tail_known = false;
        std::vector<ChangeBatch> batches;
        bool                     has_more = false;
        bool                     ok       = true;
        std::string              error;
        /// \brief Optional machine-readable sync-level error code.
        /// \details \c None means there is no structured sync classification;
        /// callers may still inspect \c ok and \c error. When set on
        /// \c ok=false responses, \c error_retryable describes whether the
        /// failure can be recovered by protocol progress such as re-pulling
        /// missing batches or resending after a fresher cursor. It does not
        /// mean blindly replaying the identical request is always useful.
        SyncResponseErrorCode    error_code = SyncResponseErrorCode::None;
        bool                     error_retryable = false;
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
        /// \brief Optional machine-readable sync-level error code.
        /// \details \c None means there is no structured sync classification;
        /// callers may still inspect \c ok and \c error. When set on
        /// \c ok=false responses, \c error_retryable describes whether the
        /// failure can be recovered by protocol progress such as re-pulling
        /// missing batches or resending after a fresher cursor. It does not
        /// mean blindly replaying the identical request is always useful.
        SyncResponseErrorCode    error_code = SyncResponseErrorCode::None;
        bool                     error_retryable = false;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_PROTOCOL_HPP_INCLUDED
