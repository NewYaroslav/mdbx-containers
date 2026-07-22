cmake_minimum_required(VERSION 3.18)

project(mdbxc_installed_lowercase_mdbx LANGUAGES CXX)

find_package(mdbx_containers CONFIG REQUIRED)

if(NOT TARGET mdbx::mdbx)
    message(FATAL_ERROR "lowercase mdbxConfig did not provide mdbx::mdbx")
endif()

add_library(installed_lowercase_mdbx_smoke OBJECT main.cpp)
target_compile_features(installed_lowercase_mdbx_smoke PRIVATE cxx_std_11)
target_link_libraries(installed_lowercase_mdbx_smoke PRIVATE
    mdbx_containers::mdbx_containers)
