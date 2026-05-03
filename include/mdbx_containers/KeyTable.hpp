#pragma once
#ifndef _MDBX_CONTAINERS_KEY_TABLE_HPP_INCLUDED
#define _MDBX_CONTAINERS_KEY_TABLE_HPP_INCLUDED

#include "common.hpp"

namespace mdbxc {

	/// \class mdbxc::KeyTable
	/// \brief Ordered key-only table persisted in MDBX.
	/// \tparam KeyT Type of the keys.
	/// \tparam Options Compile-time table policy.
	template<class KeyT, class Options = DefaultTableOptions>
	class KeyTable { /* ... */ };

}; // mdbxc

#endif // _MDBX_CONTAINERS_KEY_TABLE_HPP_INCLUDED
