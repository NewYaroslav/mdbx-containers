#pragma once
#ifndef _MDBX_CONTAINERS_KEY_MULTI_VALUE_TABLE_HPP_INCLUDED
#define _MDBX_CONTAINERS_KEY_MULTI_VALUE_TABLE_HPP_INCLUDED

#include "common.hpp"

namespace mdbxc {

    /// \class mdbxc::KeyMultiValueTable
    /// \brief Multi-value table (duplicate keys allowed) persisted in MDBX.
    /// \tparam KeyT Type of the keys.
    /// \tparam ValueT Type of the values.
    /// \tparam Options Compile-time table policy.
    template<class KeyT, class ValueT, class Options = DefaultTableOptions>
    class KeyMultiValueTable { /* ... */ };

}; // namespace mdbxc

#endif // _MDBX_CONTAINERS_KEY_MULTI_VALUE_TABLE_HPP_INCLUDED
