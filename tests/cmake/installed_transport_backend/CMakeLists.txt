cmake_minimum_required(VERSION 3.18)

project(mdbxc_installed_transport_backend_smoke LANGUAGES CXX)

find_package(mdbx_containers CONFIG REQUIRED)

if(NOT COMMAND mdbx_containers_simple_web_http_transport_provide)
    message(FATAL_ERROR
        "mdbx_containers_simple_web_http_transport_provide was not exported")
endif()
if(NOT COMMAND mdbx_containers_simple_web_websocket_transport_provide)
    message(FATAL_ERROR
        "mdbx_containers_simple_web_websocket_transport_provide was not exported")
endif()
if(NOT COMMAND mdbx_containers_kurlyk_http_transport_provide)
    message(FATAL_ERROR
        "mdbx_containers_kurlyk_http_transport_provide was not exported")
endif()

mdbx_containers_simple_web_http_transport_provide(
    OUT_TARGET MDBXC_SIMPLE_WEB_HTTP_TARGET)
mdbx_containers_simple_web_websocket_transport_provide(
    OUT_TARGET MDBXC_SIMPLE_WEB_WEBSOCKET_TARGET)
mdbx_containers_kurlyk_http_transport_provide(
    OUT_TARGET MDBXC_KURLYK_HTTP_TARGET)

if(NOT TARGET ${MDBXC_SIMPLE_WEB_HTTP_TARGET})
    message(FATAL_ERROR
        "Simple-Web HTTP transport target was not created")
endif()
if(NOT TARGET ${MDBXC_SIMPLE_WEB_WEBSOCKET_TARGET})
    message(FATAL_ERROR
        "Simple-WebSocket transport target was not created")
endif()
if(NOT TARGET ${MDBXC_KURLYK_HTTP_TARGET})
    message(FATAL_ERROR
        "Kurlyk HTTP transport target was not created")
endif()

add_library(installed_simple_web_http_transport_smoke OBJECT
    simple_web_http.cpp)
target_compile_features(installed_simple_web_http_transport_smoke PRIVATE
    cxx_std_11)
target_link_libraries(installed_simple_web_http_transport_smoke PRIVATE
    ${MDBXC_SIMPLE_WEB_HTTP_TARGET})

add_library(installed_simple_web_websocket_transport_smoke OBJECT
    simple_web_websocket.cpp)
target_compile_features(installed_simple_web_websocket_transport_smoke
    PRIVATE cxx_std_11)
target_link_libraries(installed_simple_web_websocket_transport_smoke PRIVATE
    ${MDBXC_SIMPLE_WEB_WEBSOCKET_TARGET})

add_library(installed_kurlyk_http_transport_smoke OBJECT
    kurlyk_http.cpp)
target_compile_features(installed_kurlyk_http_transport_smoke PRIVATE
    cxx_std_17)
target_link_libraries(installed_kurlyk_http_transport_smoke PRIVATE
    ${MDBXC_KURLYK_HTTP_TARGET})
