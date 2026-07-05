cmake_minimum_required(VERSION 3.18)

project(mdbxc_parent_targets_smoke LANGUAGES CXX)

set(MDBXC_PARENT_TARGET_CASE "" CACHE STRING "Parent-provided MDBX target case")
set_property(CACHE MDBXC_PARENT_TARGET_CASE PROPERTY STRINGS
    mdbx
    libmdbx
    mdbx-static
    mdbx-static-bare
    libmdbx-static
    libmdbx-static-bare
)

if(MDBXC_PARENT_TARGET_CASE STREQUAL "")
    message(FATAL_ERROR "Set MDBXC_PARENT_TARGET_CASE to one of: mdbx, libmdbx, mdbx-static, mdbx-static-bare, libmdbx-static, libmdbx-static-bare")
endif()

if(MDBXC_PARENT_TARGET_CASE STREQUAL "mdbx")
    add_library(parent_mdbx INTERFACE)
    add_library(mdbx::mdbx ALIAS parent_mdbx)
elseif(MDBXC_PARENT_TARGET_CASE STREQUAL "libmdbx")
    add_library(parent_libmdbx INTERFACE)
    add_library(libmdbx::mdbx ALIAS parent_libmdbx)
elseif(MDBXC_PARENT_TARGET_CASE STREQUAL "mdbx-static")
    add_library(parent_mdbx_static INTERFACE)
    add_library(mdbx::mdbx-static ALIAS parent_mdbx_static)
elseif(MDBXC_PARENT_TARGET_CASE STREQUAL "mdbx-static-bare")
    add_library(mdbx-static INTERFACE)
elseif(MDBXC_PARENT_TARGET_CASE STREQUAL "libmdbx-static")
    add_library(parent_libmdbx_static INTERFACE)
    add_library(libmdbx::mdbx-static ALIAS parent_libmdbx_static)
elseif(MDBXC_PARENT_TARGET_CASE STREQUAL "libmdbx-static-bare")
    add_library(libmdbx-static INTERFACE)
else()
    message(FATAL_ERROR "Unsupported MDBXC_PARENT_TARGET_CASE='${MDBXC_PARENT_TARGET_CASE}'")
endif()

set(MDBXC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MDBXC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MDBXC_USE_ASAN OFF CACHE BOOL "" FORCE)
set(MDBXC_DEPS_MODE BUNDLED CACHE STRING "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." mdbx-containers-build)

if(NOT TARGET mdbx_containers::mdbx_containers)
    message(FATAL_ERROR "mdbx_containers::mdbx_containers target was not created")
endif()

if(NOT TARGET mdbx::mdbx)
    message(FATAL_ERROR "Canonical mdbx::mdbx target was not created")
endif()

if(MDBXC_PARENT_TARGET_CASE MATCHES "static" AND NOT TARGET mdbx::mdbx-static)
    message(FATAL_ERROR "Canonical mdbx::mdbx-static target was not created")
endif()

add_executable(parent_target_smoke main.cpp)
target_link_libraries(parent_target_smoke PRIVATE mdbx_containers::mdbx_containers)
