# cmake/deps/kurlyk.cmake
# Provides a header-only Kurlyk HTTP client target for optional sync transport
# examples. The target intentionally enables only Kurlyk HTTP support; WebSocket,
# auth helpers, and OAuth helpers stay disabled so this dependency remains a
# small libcurl-backed client backend.
#
# Public API:
#   kurlyk_http_client_provide(
#       OUT_TARGET <var>     # returns the INTERFACE target name to link against
#   )

include(CMakeParseArguments)
include(FetchContent)

set(MDBXC_KURLYK_GIT_TAG
    "v1.0.1" CACHE STRING
    "Kurlyk tag used by the optional HTTP sync client backend.")
set(MDBXC_MINGW_CURL_GIT_TAG
    "840e56c6b2a11076ad5d15526bd2d4cedc8cdb9d" CACHE STRING
    "Ready-made Win64 libcurl package commit used by the MinGW Kurlyk HTTP example fallback.")

function(_mdbxc_kurlyk_fetchcontent_populate name)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(${name})
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endfunction()

function(kurlyk_http_client_provide)
    set(_options)
    set(_one_value OUT_TARGET)
    set(_multi_value)
    cmake_parse_arguments(KURLYK "${_options}" "${_one_value}"
        "${_multi_value}" ${ARGN})

    if(NOT KURLYK_OUT_TARGET)
        message(FATAL_ERROR
            "kurlyk_http_client_provide requires OUT_TARGET <var>")
    endif()

    FetchContent_Declare(
        mdbxc_kurlyk
        GIT_REPOSITORY https://github.com/NewYaroslav/kurlyk.git
        GIT_TAG        ${MDBXC_KURLYK_GIT_TAG}
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(mdbxc_kurlyk)
    if(NOT mdbxc_kurlyk_POPULATED)
        _mdbxc_kurlyk_fetchcontent_populate(mdbxc_kurlyk)
        FetchContent_GetProperties(mdbxc_kurlyk)
    endif()

    if(NOT EXISTS "${mdbxc_kurlyk_SOURCE_DIR}/include/kurlyk.hpp")
        message(FATAL_ERROR
            "Kurlyk headers were not found after FetchContent")
    endif()

    if(NOT TARGET mdbxc_kurlyk_http_client)
        find_package(CURL QUIET)
        if(NOT TARGET CURL::libcurl)
            if(WIN32 AND MINGW AND
                    MDBXC_KURLYK_HTTP_SYNC_MINGW_CURL_FALLBACK)
                mdbxc_mingw_curl_fallback_provide()
            else()
                message(FATAL_ERROR
                    "libcurl was not found. Install libcurl, set "
                    "CURL_LIBRARY/CURL_INCLUDE_DIR, or enable "
                    "MDBXC_KURLYK_HTTP_SYNC_MINGW_CURL_FALLBACK for the "
                    "MinGW Kurlyk HTTP example.")
            endif()
        endif()
        find_package(Threads REQUIRED)

        add_library(mdbxc_kurlyk_http_client INTERFACE)
        target_include_directories(mdbxc_kurlyk_http_client INTERFACE
            "${mdbxc_kurlyk_SOURCE_DIR}/include")
        target_compile_features(mdbxc_kurlyk_http_client INTERFACE
            cxx_std_17)
        target_compile_definitions(mdbxc_kurlyk_http_client INTERFACE
            KURLYK_HTTP_SUPPORT=1
            KURLYK_WEBSOCKET_SUPPORT=0
            KURLYK_AUTH_SUPPORT=0
            KURLYK_OAUTH_SUPPORT=0)
        target_link_libraries(mdbxc_kurlyk_http_client INTERFACE
            CURL::libcurl
            Threads::Threads)
        if(WIN32)
            target_link_libraries(mdbxc_kurlyk_http_client INTERFACE
                ws2_32
                wsock32)
        endif()
    endif()

    set(${KURLYK_OUT_TARGET} mdbxc_kurlyk_http_client PARENT_SCOPE)
endfunction()

function(mdbxc_mingw_curl_fallback_provide)
    FetchContent_Declare(
        mdbxc_curl_win64_mingw
        GIT_REPOSITORY https://github.com/NewYaroslav/curl-8.11.0_1-win64-mingw.git
        GIT_TAG        ${MDBXC_MINGW_CURL_GIT_TAG}
    )
    FetchContent_GetProperties(mdbxc_curl_win64_mingw)
    if(NOT mdbxc_curl_win64_mingw_POPULATED)
        _mdbxc_kurlyk_fetchcontent_populate(mdbxc_curl_win64_mingw)
        FetchContent_GetProperties(mdbxc_curl_win64_mingw)
    endif()

    set(_mdbxc_curl_implib
        "${mdbxc_curl_win64_mingw_SOURCE_DIR}/lib/libcurl.dll.a")
    set(_mdbxc_curl_runtime
        "${mdbxc_curl_win64_mingw_SOURCE_DIR}/bin/libcurl-x64.dll")
    set(_mdbxc_curl_include
        "${mdbxc_curl_win64_mingw_SOURCE_DIR}/include")

    if(NOT EXISTS "${_mdbxc_curl_implib}" OR
            NOT EXISTS "${_mdbxc_curl_runtime}" OR
            NOT EXISTS "${_mdbxc_curl_include}/curl/curl.h")
        message(FATAL_ERROR
            "Ready-made MinGW libcurl fallback package has unexpected layout")
    endif()

    if(NOT TARGET CURL::libcurl)
        add_library(CURL::libcurl SHARED IMPORTED GLOBAL)
        set_target_properties(CURL::libcurl PROPERTIES
            IMPORTED_IMPLIB "${_mdbxc_curl_implib}"
            IMPORTED_LOCATION "${_mdbxc_curl_runtime}"
            INTERFACE_INCLUDE_DIRECTORIES "${_mdbxc_curl_include}")
    endif()

    set_property(GLOBAL APPEND PROPERTY
        MDBXC_MINGW_CURL_RUNTIME_DLLS "${_mdbxc_curl_runtime}")
    message(STATUS
        "CURL: using MinGW fallback package ${MDBXC_MINGW_CURL_GIT_TAG}")
endfunction()
