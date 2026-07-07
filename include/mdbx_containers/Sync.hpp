#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_HPP_INCLUDED

/// \file Sync.hpp
/// \brief Aggregate header for the optional sync subsystem.
/// \details
/// Pulls in all foundation types and codec when \c MDBXC_SYNC_ENABLED is
/// non-zero. Otherwise the include is a no-op so applications that do not
/// need replication pay zero compile-time or runtime cost.

#include "sync/SyncModule.hpp"

#if MDBXC_SYNC_ENABLED
#include "sync/ChangeBatch.hpp"
#include "sync/ChangeBatchCodec.hpp"
#include "sync/ChangeOp.hpp"
#include "sync/CodecBounds.hpp"
#include "sync/CodecFlags.hpp"
#include "sync/Common.hpp"
#include "sync/ConflictPolicy.hpp"
#include "sync/IdentityProvider.hpp"
#include "sync/ISyncPeer.hpp"
#include "sync/Protocol.hpp"
#include "sync/SyncCursor.hpp"
#endif

#endif // MDBX_CONTAINERS_HEADER_SYNC_HPP_INCLUDED