#pragma once
#ifndef MDBX_CONTAINERS_HEADER_MDBX_CONTAINERS_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_MDBX_CONTAINERS_HPP_INCLUDED

/// \file mdbx_containers.hpp
/// \brief Main include file for the MDBX Containers library. Pulls in every
///        public header: table wrappers, the sync subsystem umbrella, and the
///        vector subsystem umbrella. Use \c mdbx_containers/tables.hpp if
///        only the table API is needed.

#include "mdbx_containers/tables.hpp"
#include "mdbx_containers/sync.hpp"
#include "mdbx_containers/vector.hpp"

#endif // MDBX_CONTAINERS_HEADER_MDBX_CONTAINERS_HPP_INCLUDED
