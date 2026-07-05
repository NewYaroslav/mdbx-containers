#pragma once
#ifndef MDBX_CONTAINERS_HEADER_TEST_ASSERT_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_TEST_ASSERT_HPP_INCLUDED

#include <stdexcept>
#include <string>

#define MDBXC_TEST_STRINGIFY_IMPL(x) #x
#define MDBXC_TEST_STRINGIFY(x) MDBXC_TEST_STRINGIFY_IMPL(x)

#define MDBXC_TEST_ASSERT(expr)                                                               \
    do {                                                                                      \
        if (!(expr)) {                                                                        \
            throw std::runtime_error(std::string(__FILE__) + ":" +                            \
                                     MDBXC_TEST_STRINGIFY(__LINE__) +                         \
                                     ": assertion failed: " #expr);                           \
        }                                                                                     \
    } while (false)

#endif // MDBX_CONTAINERS_HEADER_TEST_ASSERT_HPP_INCLUDED
