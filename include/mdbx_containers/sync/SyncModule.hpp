#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_SYNC_MODULE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_SYNC_MODULE_HPP_INCLUDED

/// \file SyncModule.hpp
/// \brief Compile-time gate for the optional sync subsystem.

#ifndef MDBXC_SYNC_ENABLED
/// \brief Define to 1 to compile in the sync subsystem (changelog, peers,
/// replication). When undefined, sync headers expand to no-op stubs and
/// \c Connection::attach_sync_engine is not available.
#define MDBXC_SYNC_ENABLED 0
#endif

#endif // MDBX_CONTAINERS_HEADER_SYNC_SYNC_MODULE_HPP_INCLUDED
