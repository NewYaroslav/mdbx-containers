cmake_minimum_required(VERSION 3.18)

project(mdbxc_installed_transport_backend_negative LANGUAGES CXX)

find_package(mdbx_containers CONFIG REQUIRED)

if(NOT MDBXC_NEGATIVE_PROVIDER)
    message(FATAL_ERROR "MDBXC_NEGATIVE_PROVIDER is required")
endif()

if(MDBXC_NEGATIVE_PROVIDER STREQUAL "simple_web_http")
    mdbx_containers_simple_web_http_transport_provide(
        OUT_TARGET MDBXC_NEGATIVE_TARGET
        UNEXPECTED_ARGUMENT)
elseif(MDBXC_NEGATIVE_PROVIDER STREQUAL "simple_web_websocket")
    mdbx_containers_simple_web_websocket_transport_provide(
        OUT_TARGET MDBXC_NEGATIVE_TARGET
        UNEXPECTED_ARGUMENT)
elseif(MDBXC_NEGATIVE_PROVIDER STREQUAL "kurlyk_http")
    mdbx_containers_kurlyk_http_transport_provide(
        OUT_TARGET MDBXC_NEGATIVE_TARGET
        UNEXPECTED_ARGUMENT)
else()
    message(FATAL_ERROR
        "unknown MDBXC_NEGATIVE_PROVIDER: ${MDBXC_NEGATIVE_PROVIDER}")
endif()

message(FATAL_ERROR
    "${MDBXC_NEGATIVE_PROVIDER} accepted an unexpected argument")
