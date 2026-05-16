#pragma once
#ifndef _MDBX_CONTAINERS_RESULT_CONTAINERS_HPP_INCLUDED
#define _MDBX_CONTAINERS_RESULT_CONTAINERS_HPP_INCLUDED

/// \file detail/ResultContainers.hpp
/// \brief Internal traits for selecting result container shapes.

#include <utility>
#include <vector>

namespace mdbxc {
namespace detail {

    template<template<class...> class ContainerT, class KeyT, class ValueT>
    struct key_value_result_container {
        typedef ContainerT<KeyT, ValueT> type;
    };

    template<class KeyT, class ValueT>
    struct key_value_result_container<std::vector, KeyT, ValueT> {
        typedef std::vector<std::pair<KeyT, ValueT> > type;
    };

} // namespace detail
} // namespace mdbxc

#endif // _MDBX_CONTAINERS_RESULT_CONTAINERS_HPP_INCLUDED
