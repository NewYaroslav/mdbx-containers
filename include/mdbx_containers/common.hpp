#pragma once
#ifndef _MDBX_CONTAINERS_COMMON_HPP_INCLUDED
#define _MDBX_CONTAINERS_COMMON_HPP_INCLUDED

/// \file common.hpp
/// \brief Publicly usable low-level components of the MDBX Containers library.
/// 
/// Includes building blocks such as Connection, Transaction, Config, and exceptions.

#include <string>
#include <vector>
#include <deque>
#include <list>
#include <array>
#include <bitset>
#include <unordered_map>
#include <type_traits>
#include <optional>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <cstdlib>
#include <mdbx.h>

#ifndef MDBX_CONTAINERS_SEPARATE_COMPILATION
#define MDBX_CONTAINERS_HEADER_ONLY
#endif

#include "common/MdbxException.hpp"
#include "common/Config.hpp"
#include "detail/TransactionTracker.hpp"
#include "detail/utils.hpp"
#include "common/Transaction.hpp"
#include "common/Connection.hpp"
#include "detail/BaseTable.hpp"

#endif // _MDBX_CONTAINERS_COMMON_HPP_INCLUDED