#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_I_SYNC_PEER_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_I_SYNC_PEER_HPP_INCLUDED

/// \file ISyncPeer.hpp
/// \brief Abstract transport-level peer used by \c SyncEngine.

#include "protocol.hpp"

namespace mdbxc {
namespace sync {

    /// \brief Abstract peer that exchanges pull and push requests.
    /// \details Concrete implementations (in-process, HTTP, WebSocket) live
    /// outside the core sync layer and depend on optional transport headers.
    /// Implementations that override \c request_cancel() must allow it to be
    /// called concurrently with \c pull() / \c push() and tolerate calls that
    /// race with operation startup or completion.
    class ISyncPeer {
    public:
        virtual ~ISyncPeer() {}

        /// \brief Sends a pull request and returns the response.
        virtual PullResponse pull(const PullRequest& request) = 0;

        /// \brief Sends a push request and returns the response.
        virtual PushResponse push(const PushRequest& request) = 0;

        /// \brief Requests cancellation of in-flight transport operations.
        /// \details Best-effort hook for blocking transports. The default
        /// implementation is a no-op, so non-interruptible peers remain valid.
        /// Overrides should return quickly and should not throw.
        virtual void request_cancel() {}
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_SYNC_I_SYNC_PEER_HPP_INCLUDED
