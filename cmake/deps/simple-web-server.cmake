# cmake/deps/simple-web-server.cmake
# Provides a header-only Simple-Web-Server target for the optional HTTP sync
# example. The target deliberately uses standalone Asio and does not run the
# upstream Simple-Web-Server CMakeLists, because that path may search for Boost
# before the standalone Asio include directory is visible.
#
# Public API:
#   simple_web_server_provide(
#       OUT_TARGET <var>     # returns the INTERFACE target name to link against
#   )

include(CMakeParseArguments)
include(FetchContent)

set(MDBXC_ASIO_GIT_TAG
    "12e0ce9e0500bf0f247dbd1ae894272656456079" CACHE STRING
    "Asio commit used by the optional HTTP example; corresponds to asio-1-30-2.")
set(MDBXC_SIMPLE_WEB_SERVER_GIT_TAG
    "898b6abd1be568ff9de4390d44288962e3fac337" CACHE STRING
    "Simple-Web-Server commit used by the optional HTTP example; corresponds to v3.1.1.")

function(_mdbxc_fetchcontent_populate name)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(${name})
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endfunction()

function(simple_web_server_provide)
    set(_options)
    set(_one_value OUT_TARGET)
    set(_multi_value)
    cmake_parse_arguments(SWS "${_options}" "${_one_value}"
        "${_multi_value}" ${ARGN})

    if(NOT SWS_OUT_TARGET)
        message(FATAL_ERROR
            "simple_web_server_provide requires OUT_TARGET <var>")
    endif()

    FetchContent_Declare(
        mdbxc_asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        ${MDBXC_ASIO_GIT_TAG}
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(mdbxc_asio)
    if(NOT mdbxc_asio_POPULATED)
        _mdbxc_fetchcontent_populate(mdbxc_asio)
        FetchContent_GetProperties(mdbxc_asio)
    endif()

    FetchContent_Declare(
        mdbxc_simple_web_server_source
        GIT_REPOSITORY https://gitlab.com/eidheim/Simple-Web-Server.git
        GIT_TAG        ${MDBXC_SIMPLE_WEB_SERVER_GIT_TAG}
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(mdbxc_simple_web_server_source)
    if(NOT mdbxc_simple_web_server_source_POPULATED)
        _mdbxc_fetchcontent_populate(mdbxc_simple_web_server_source)
        FetchContent_GetProperties(mdbxc_simple_web_server_source)
    endif()

    if(NOT EXISTS "${mdbxc_asio_SOURCE_DIR}/asio/include/asio.hpp")
        message(FATAL_ERROR
            "Standalone Asio headers were not found after FetchContent")
    endif()
    if(NOT EXISTS
            "${mdbxc_simple_web_server_source_SOURCE_DIR}/server_http.hpp")
        message(FATAL_ERROR
            "Simple-Web-Server headers were not found after FetchContent")
    endif()

    if(NOT TARGET mdbxc_simple_web_server)
        add_library(mdbxc_simple_web_server INTERFACE)
        target_include_directories(mdbxc_simple_web_server INTERFACE
            "${mdbxc_simple_web_server_source_SOURCE_DIR}"
            "${mdbxc_asio_SOURCE_DIR}/asio/include")
        target_compile_definitions(mdbxc_simple_web_server INTERFACE
            ASIO_STANDALONE=1
            USE_STANDALONE_ASIO=1)

        find_package(Threads REQUIRED)
        target_link_libraries(mdbxc_simple_web_server INTERFACE
            Threads::Threads)
        if(WIN32)
            target_link_libraries(mdbxc_simple_web_server INTERFACE
                ws2_32
                wsock32)
        endif()
    endif()

    set(${SWS_OUT_TARGET} mdbxc_simple_web_server PARENT_SCOPE)
endfunction()
