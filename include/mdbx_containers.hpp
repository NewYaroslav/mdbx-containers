#pragma once
#ifndef MDBX_CONTAINERS_HEADER_MDBX_CONTAINERS_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_MDBX_CONTAINERS_HPP_INCLUDED

/// \file mdbx_containers.hpp
/// \brief Main include file for the MDBX Containers library.
/// 
/// Provides integration between MDBX and STL-style containers with support
/// for transactions, persistence, and thread-bound table operations.

#include "mdbx_containers/AnyValueTable.hpp"
#include "mdbx_containers/Hash.hpp"
#include "mdbx_containers/HashedKeyValueStore.hpp"
#include "mdbx_containers/KeyMultiValueTable.hpp"
#include "mdbx_containers/KeyTable.hpp"
#include "mdbx_containers/KeyValueTable.hpp"
#include "mdbx_containers/SequenceTable.hpp"
#include "mdbx_containers/ValueTable.hpp"

#endif // MDBX_CONTAINERS_HEADER_MDBX_CONTAINERS_HPP_INCLUDED
