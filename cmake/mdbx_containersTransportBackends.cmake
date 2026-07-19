include_guard(GLOBAL)

include(CMakeParseArguments)

function(_mdbxc_transport_backend_parse_out function_name out_var)
    set(_options)
    set(_one_value OUT_TARGET)
    set(_multi_value)
    cmake_parse_arguments(MDBXC_TRANSPORT "${_options}" "${_one_value}"
        "${_multi_value}" ${ARGN})

    if(NOT MDBXC_TRANSPORT_OUT_TARGET)
        message(FATAL_ERROR "${function_name} requires OUT_TARGET <var>")
    endif()

    set(${out_var} "${MDBXC_TRANSPORT_OUT_TARGET}" PARENT_SCOPE)
endfunction()

function(mdbx_containers_simple_web_http_transport_provide)
    _mdbxc_transport_backend_parse_out(
        "mdbx_containers_simple_web_http_transport_provide"
        _out_var ${ARGN})

    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deps/simple-web-server.cmake")
    simple_web_server_provide(OUT_TARGET _mdbxc_sws_target)

    if(NOT TARGET mdbx_containers::simple_web_http_transport)
        add_library(mdbx_containers_simple_web_http_transport INTERFACE)
        target_compile_features(mdbx_containers_simple_web_http_transport
            INTERFACE cxx_std_11)
        target_compile_definitions(mdbx_containers_simple_web_http_transport
            INTERFACE MDBXC_SYNC_ENABLED=1)
        target_link_libraries(mdbx_containers_simple_web_http_transport
            INTERFACE
                mdbx_containers::mdbx_containers
                ${_mdbxc_sws_target})
        add_library(mdbx_containers::simple_web_http_transport ALIAS
            mdbx_containers_simple_web_http_transport)
    endif()

    set(${_out_var} mdbx_containers::simple_web_http_transport PARENT_SCOPE)
endfunction()

function(mdbx_containers_simple_web_websocket_transport_provide)
    _mdbxc_transport_backend_parse_out(
        "mdbx_containers_simple_web_websocket_transport_provide"
        _out_var ${ARGN})

    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deps/simple-web-server.cmake")
    simple_websocket_server_provide(OUT_TARGET _mdbxc_sws_ws_target)

    if(NOT TARGET mdbx_containers::simple_web_websocket_transport)
        add_library(mdbx_containers_simple_web_websocket_transport INTERFACE)
        target_compile_features(
            mdbx_containers_simple_web_websocket_transport
            INTERFACE cxx_std_11)
        target_compile_definitions(
            mdbx_containers_simple_web_websocket_transport
            INTERFACE MDBXC_SYNC_ENABLED=1)
        target_link_libraries(
            mdbx_containers_simple_web_websocket_transport
            INTERFACE
                mdbx_containers::mdbx_containers
                ${_mdbxc_sws_ws_target})
        add_library(mdbx_containers::simple_web_websocket_transport ALIAS
            mdbx_containers_simple_web_websocket_transport)
    endif()

    set(${_out_var} mdbx_containers::simple_web_websocket_transport
        PARENT_SCOPE)
endfunction()

function(mdbx_containers_kurlyk_http_transport_provide)
    _mdbxc_transport_backend_parse_out(
        "mdbx_containers_kurlyk_http_transport_provide"
        _out_var ${ARGN})

    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deps/kurlyk.cmake")
    kurlyk_http_client_provide(OUT_TARGET _mdbxc_kurlyk_target)

    if(NOT TARGET mdbx_containers::kurlyk_http_transport)
        add_library(mdbx_containers_kurlyk_http_transport INTERFACE)
        target_compile_features(mdbx_containers_kurlyk_http_transport
            INTERFACE cxx_std_17)
        target_compile_definitions(mdbx_containers_kurlyk_http_transport
            INTERFACE MDBXC_SYNC_ENABLED=1)
        target_link_libraries(mdbx_containers_kurlyk_http_transport
            INTERFACE
                mdbx_containers::mdbx_containers
                ${_mdbxc_kurlyk_target})
        add_library(mdbx_containers::kurlyk_http_transport ALIAS
            mdbx_containers_kurlyk_http_transport)
    endif()

    set(${_out_var} mdbx_containers::kurlyk_http_transport PARENT_SCOPE)
endfunction()
