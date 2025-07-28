cmake_minimum_required(VERSION 3.18)
project(mdbx_containers LANGUAGES CXX)

# Папка для статических библиотек (.a, .lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib CACHE PATH "")

# Папка для динамических библиотек (.so, .dll)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib CACHE PATH "")

option(BUILD_DEPS "Build internal dependencies (e.g., MDBX)" OFF)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_STATIC_LIB "Build static library from cpp/ipp sources" OFF)

message(STATUS "BUILD_STATIC_LIB = ${BUILD_STATIC_LIB}")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# MDBX: либо внешний, либо внутренний
if(BUILD_DEPS)
    add_subdirectory(libs)
else()
    find_package(mdbx REQUIRED)
endif()

# Static library (optional)
if(BUILD_STATIC_LIB)
    file(GLOB_RECURSE LIB_SOURCES src/*.cpp)
    add_library(mdbx_containers STATIC ${LIB_SOURCES})
    target_include_directories(mdbx_containers PUBLIC include)
    target_link_libraries(mdbx_containers PUBLIC mdbx-static)
    target_compile_definitions(mdbx_containers PRIVATE MDBX_CONTAINERS_SEPARATE_COMPILATION)
endif()

# Examples
if(BUILD_EXAMPLES)
    file(GLOB EXAMPLES examples/*.cpp)
    foreach(example_file ${EXAMPLES})
        get_filename_component(example_name ${example_file} NAME_WE)
        add_executable(${example_name} ${example_file})
        if(BUILD_STATIC_LIB)
            target_link_libraries(${example_name} PRIVATE mdbx_containers)
        endif()
    endforeach()
endif()

# Tests
if(BUILD_TESTS)
    set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)
    file(GLOB TESTS tests/*.cpp)
    foreach(test_file ${TESTS})
        get_filename_component(test_name ${test_file} NAME_WE)
        add_executable(${test_name} ${test_file})

        set_target_properties(${test_name} PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS FALSE
        )
        set_target_properties(${test_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
        )

        if(BUILD_STATIC_LIB)
            # target_compile_definitions(${test_name} PRIVATE MDBX_CONTAINERS_SEPARATE_COMPILATION)
            target_link_libraries(${test_name} PRIVATE mdbx_containers)
            message(STATUS "Linking test ${test_name} with mdbx_containers")
        else()
            target_link_libraries(${test_name} PRIVATE mdbx-static)
        endif()
    endforeach()
endif()

# Headers
install(DIRECTORY include/ DESTINATION include)

# Static lib
if(TARGET mdbx_containers)
    install(TARGETS mdbx_containers ARCHIVE DESTINATION lib)
endif()

# Копируем include в build/include
add_custom_target(copy_headers_to_build ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_BINARY_DIR}/include
    COMMENT "Copying headers to build/include"
)

