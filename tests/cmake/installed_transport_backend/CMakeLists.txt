cmake_minimum_required(VERSION 3.18)

project(mdbxc_installed_transport_backend_smoke LANGUAGES CXX)

find_package(mdbx_containers CONFIG REQUIRED)

if(NOT COMMAND mdbx_containers_simple_web_http_transport_provide)
    message(FATAL_ERROR
        "mdbx_containers_simple_web_http_transport_provide was not exported")
endif()

mdbx_containers_simple_web_http_transport_provide(
    OUT_TARGET MDBXC_SIMPLE_WEB_HTTP_TARGET)

if(NOT TARGET ${MDBXC_SIMPLE_WEB_HTTP_TARGET})
    message(FATAL_ERROR
        "Simple-Web HTTP transport target was not created")
endif()

add_library(installed_transport_backend_smoke OBJECT main.cpp)
target_compile_features(installed_transport_backend_smoke PRIVATE cxx_std_11)
target_link_libraries(installed_transport_backend_smoke PRIVATE
    ${MDBXC_SIMPLE_WEB_HTTP_TARGET})
