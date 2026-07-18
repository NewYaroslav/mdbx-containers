# cmake/deps/openssl-mingw.cmake
# Provides OpenSSL::Crypto from a ready-made Win64 package for the optional
# WebSocket example when MinGW cannot find a system OpenSSL package.

include(FetchContent)

set(MDBXC_OPENSSL_WIN64_GIT_TAG
    "635bbfe81230fcf4c3579f9e9f13fdeff71e8d70" CACHE STRING
    "OpenSSL Win64 package commit used by the optional WebSocket example.")

function(mdbxc_mingw_openssl_fallback_provide)
    if(NOT WIN32 OR NOT MINGW)
        message(FATAL_ERROR
            "mdbxc_mingw_openssl_fallback_provide is only for MinGW on Windows")
    endif()

    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "The bundled OpenSSL fallback is Win64-only")
    endif()

    FetchContent_Declare(
        mdbxc_openssl_win64
        GIT_REPOSITORY https://github.com/NewYaroslav/openssl-win64-v3.4.0.git
        GIT_TAG        ${MDBXC_OPENSSL_WIN64_GIT_TAG}
    )
    FetchContent_GetProperties(mdbxc_openssl_win64)
    if(NOT mdbxc_openssl_win64_POPULATED)
        if(COMMAND _mdbxc_fetchcontent_populate)
            _mdbxc_fetchcontent_populate(mdbxc_openssl_win64)
        else()
            FetchContent_Populate(mdbxc_openssl_win64)
        endif()
        FetchContent_GetProperties(mdbxc_openssl_win64)
    endif()

    set(_openssl_include
        "${mdbxc_openssl_win64_SOURCE_DIR}/include")
    set(_openssl_lib_dir
        "${mdbxc_openssl_win64_SOURCE_DIR}/lib/VC/x64/MD")
    set(_openssl_bin_dir
        "${mdbxc_openssl_win64_SOURCE_DIR}/bin")
    set(_openssl_crypto_lib "${_openssl_lib_dir}/libcrypto.lib")
    set(_openssl_crypto_dll "${_openssl_bin_dir}/libcrypto-3-x64.dll")

    foreach(_required_path IN ITEMS
            "${_openssl_include}/openssl/crypto.h"
            "${_openssl_crypto_lib}"
            "${_openssl_crypto_dll}")
        if(NOT EXISTS "${_required_path}")
            message(FATAL_ERROR
                "OpenSSL Win64 fallback is missing ${_required_path}")
        endif()
    endforeach()

    if(NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto SHARED IMPORTED GLOBAL)
        set_target_properties(OpenSSL::Crypto PROPERTIES
            IMPORTED_IMPLIB "${_openssl_crypto_lib}"
            IMPORTED_LOCATION "${_openssl_crypto_dll}"
            INTERFACE_INCLUDE_DIRECTORIES "${_openssl_include}")
    endif()

    set_property(GLOBAL PROPERTY MDBXC_MINGW_OPENSSL_RUNTIME_DLLS
        "${_openssl_crypto_dll}")
    message(STATUS
        "OpenSSL Crypto: using MinGW fallback from openssl-win64-v3.4.0")
endfunction()
