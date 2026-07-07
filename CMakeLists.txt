cmake_minimum_required(VERSION 3.18)
project(mdbx_containers VERSION 1.0.2 LANGUAGES CXX)

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)

# ---------------------------------------
# Output directories
# ---------------------------------------
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib CACHE PATH "")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib CACHE PATH "")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin CACHE PATH "")

# ---------------------------------------
# Options (prefixed to avoid clashes when used as a subproject)
# ---------------------------------------
option(MDBXC_BUILD_EXAMPLES     "Build examples"                                  ON)
option(MDBXC_BUILD_TESTS        "Build tests with CTest"                           ON)
option(MDBXC_USE_ASAN "Enable AddressSanitizer for tests/examples (not for libmdbx)" ON)

# Three-mode dependency policy for MDBX: AUTO | SYSTEM | BUNDLED
set(MDBXC_DEPS_MODE "AUTO" CACHE STRING "Dependency mode for MDBX: AUTO|SYSTEM|BUNDLED")
set_property(CACHE MDBXC_DEPS_MODE PROPERTY STRINGS AUTO SYSTEM BUNDLED)

message(STATUS "MDBXC_DEPS_MODE         = ${MDBXC_DEPS_MODE}")
message(STATUS "MDBXC_BUILD_EXAMPLES    = ${MDBXC_BUILD_EXAMPLES}")
message(STATUS "MDBXC_BUILD_TESTS       = ${MDBXC_BUILD_TESTS}")
message(STATUS "MDBXC_USE_ASAN          = ${MDBXC_USE_ASAN}")

set(MDBXC_HAS_ASAN OFF CACHE BOOL "Compiler+linker support -fsanitize=address")

if (MDBXC_USE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang") AND NOT MSVC)
    # Check whether the compiler supports the flag
    check_cxx_compiler_flag("-fsanitize=address" MDBXC_ASAN_CFLAG_OK)
    # Check whether the linker supports the flag
    check_linker_flag(CXX "-fsanitize=address" MDBXC_ASAN_LDFLAG_OK)

    if (MDBXC_ASAN_CFLAG_OK AND MDBXC_ASAN_LDFLAG_OK)
        set(MDBXC_HAS_ASAN ON CACHE BOOL "" FORCE)
    else()
        message(WARNING "ASan is unavailable for the current toolchain (compiler: ${MDBXC_ASAN_CFLAG_OK}, linker: ${MDBXC_ASAN_LDFLAG_OK}). Disabling MDBXC_USE_ASAN.")
        set(MDBXC_HAS_ASAN OFF CACHE BOOL "" FORCE)
    endif()
endif()

function(mdbxc_enable_asan tgt)
    if(MDBXC_HAS_ASAN AND (CMAKE_BUILD_TYPE MATCHES "Debug|RelWithDebInfo")
       AND (MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_compile_options(${tgt} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE -fsanitize=address)
    endif()
endfunction()


# ---------------------------------------
# Include paths for our headers
# ---------------------------------------
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------
# MDBX dependency via helper script
# Expects cmake/deps/mdbx.cmake to provide:
#   mdbx_provide(MODE <AUTO|SYSTEM|BUNDLED> OUT_TARGET_STATIC <var> OUT_TARGET_SHARED <var>)
# and to normalize targets:
#   mdbx::mdbx        (shared/normal)
#   mdbx::mdbx-static (static)
# ---------------------------------------
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake/deps)
include(cmake/deps/mdbx.cmake)

mdbx_provide(
    MODE ${MDBXC_DEPS_MODE}
    OUT_TARGET_STATIC MDBX_STATIC_TARGET
    OUT_TARGET_SHARED MDBX_SHARED_TARGET
)

if(NOT TARGET mdbx::mdbx AND NOT TARGET mdbx::mdbx-static)
    message(FATAL_ERROR "MDBX targets not provided by mdbx_provide(). Check cmake/deps/mdbx.cmake.")
endif()

# ---------------------------------------
# Header-only library target
# ---------------------------------------
set(_PKG_TARGET mdbx_containers)
set(_PKG_EXPORT_TARGET mdbx_containers_export)

add_library(${_PKG_TARGET} INTERFACE)
target_include_directories(${_PKG_TARGET} INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(${_PKG_TARGET} INTERFACE cxx_std_11)

if(TARGET mdbx::mdbx-static)
    set(_MDBXC_BUILD_MDBX_TARGET mdbx::mdbx-static)
else()
    set(_MDBXC_BUILD_MDBX_TARGET mdbx::mdbx)
endif()
target_link_libraries(${_PKG_TARGET} INTERFACE
    $<BUILD_INTERFACE:${_MDBXC_BUILD_MDBX_TARGET}>
    $<INSTALL_INTERFACE:mdbx::mdbx>
)

# Namespace alias for consumers
add_library(mdbx_containers::mdbx_containers ALIAS ${_PKG_TARGET})

# Install a separate export-only target so the build tree can link the bundled
# MDBX target directly, while installed packages depend on find_dependency(MDBX).
add_library(${_PKG_EXPORT_TARGET} INTERFACE)
target_include_directories(${_PKG_EXPORT_TARGET} INTERFACE
    $<INSTALL_INTERFACE:include>
)
target_compile_features(${_PKG_EXPORT_TARGET} INTERFACE cxx_std_11)
target_link_libraries(${_PKG_EXPORT_TARGET} INTERFACE
    $<INSTALL_INTERFACE:$<1:mdbx::mdbx>>
)
set_target_properties(${_PKG_EXPORT_TARGET} PROPERTIES
    EXPORT_NAME mdbx_containers
)

# ---------------------------------------
# Examples (allow interactive-only locally if needed)
# ---------------------------------------
if(MDBXC_BUILD_EXAMPLES)
    file(GLOB EXAMPLES CONFIGURE_DEPENDS examples/*.cpp)
    foreach(example_file IN LISTS EXAMPLES)
        get_filename_component(example_name ${example_file} NAME_WE)
        add_executable(${example_name} ${example_file})
        set_target_properties(${example_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/examples
        )
        target_compile_features(${example_name} PRIVATE cxx_std_11)

        target_link_libraries(${example_name} PRIVATE ${_PKG_TARGET})
        message(STATUS "Example ${example_name} -> ${_PKG_TARGET}")

        # Enable the optional sync subsystem when its headers are exercised.
        target_compile_definitions(${example_name} PRIVATE MDBXC_SYNC_ENABLED=1)

        # To enable pause-on-Enter in examples only (not in tests), uncomment:
        # target_compile_definitions(${example_name} PRIVATE INTERACTIVE_TEST)
    endforeach()
endif()

# ---------------------------------------
# Tests (non-interactive; registered with CTest)
# ---------------------------------------
if(MDBXC_BUILD_TESTS)
    include(CTest)
    enable_testing()
    file(GLOB TESTS CONFIGURE_DEPENDS tests/*.cpp)
    foreach(test_file IN LISTS TESTS)
        get_filename_component(test_name ${test_file} NAME_WE)
        add_executable(${test_name} ${test_file})
        set_target_properties(${test_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests
        )
        target_compile_features(${test_name} PRIVATE cxx_std_11)
        
        mdbxc_enable_asan(${test_name})

        target_link_libraries(${test_name} PRIVATE ${_PKG_TARGET})
        message(STATUS "Test ${test_name} -> ${_PKG_TARGET}")

        # Enable the optional sync subsystem when its headers are exercised.
        target_compile_definitions(${test_name} PRIVATE MDBXC_SYNC_ENABLED=1)

        # IMPORTANT: we do NOT define INTERACTIVE_TEST for tests.
        add_test(NAME ${test_name} COMMAND ${test_name})
        
        if(MDBXC_HAS_ASAN)
            set_tests_properties(${test_name} PROPERTIES
                ENVIRONMENT "ASAN_OPTIONS=abort_on_error=1,detect_leaks=0,allocator_may_return_null=1"
            )
        endif()
        
    endforeach()
endif()

# ---------------------------------------
# Install: headers + target + CMake package
# ---------------------------------------
install(DIRECTORY include/ DESTINATION include)

install(TARGETS ${_PKG_EXPORT_TARGET}
    EXPORT mdbx_containersTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

include(CMakePackageConfigHelpers)

set(_PKG_CONFIG_INSTALL_DIR "lib/cmake/mdbx_containers")
set(_PKG_INCLUDE_INSTALL_DIR "include")

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mdbx_containersConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mdbx_containersConfig.cmake
    INSTALL_DESTINATION ${_PKG_CONFIG_INSTALL_DIR}
    PATH_VARS _PKG_INCLUDE_INSTALL_DIR
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mdbx_containersConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT mdbx_containersTargets
    FILE mdbx_containersTargets.cmake
    NAMESPACE mdbx_containers::
    DESTINATION ${_PKG_CONFIG_INSTALL_DIR}
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mdbx_containersConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mdbx_containersConfigVersion.cmake
    DESTINATION ${_PKG_CONFIG_INSTALL_DIR}
)

# ---------------------------------------
# Developer helper: copy headers to build/include
# ---------------------------------------
add_custom_target(copy_headers_to_build ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_BINARY_DIR}/include
    COMMENT "Copying headers to build/include"
)
