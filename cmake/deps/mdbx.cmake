# cmake/deps/mdbx.cmake
# Provides libmdbx targets with minimal, robust logic:
#   1) SYSTEM/AUTO: try find_package (CONFIG/MODULE, various names)
#   2) BUNDLED/AUTO: try submodule at libs/libmdbx (with Windows/MSYS quirks)
#   3) fallback: FetchContent from upstream
#
# Canonical aliases produced:
#   mdbx::mdbx        - "the" MDBX target (shared if system provides; otherwise static alias)
#   mdbx::mdbx-static - static target (if available; else absent)
#
# Public API:
#   mdbx_provide(
#       MODE <AUTO|SYSTEM|BUNDLED>
#       OUT_TARGET_STATIC <var>     # returns 'mdbx::mdbx-static' if exists, else 'mdbx::mdbx'
#       OUT_TARGET_SHARED <var>     # returns 'mdbx::mdbx' (shared if system provided; bundled path -> static alias)
#   )
#
# NOTE: We intentionally keep bundled builds STATIC (MDBX_BUILD_SHARED_LIBRARY=OFF),
#       to preserve existing CI behavior and Windows/MSYS quirks you had.

include(FetchContent)

# -------- internals ----------------------------------------------------------

function(_mdbx_make_aliases _maybe_shared _maybe_static)
    if(DEFINED _maybe_shared AND NOT "${_maybe_shared}" STREQUAL "")
        if(TARGET ${_maybe_shared} AND NOT TARGET mdbx::mdbx)
            add_library(mdbx::mdbx ALIAS ${_maybe_shared})
            message(STATUS "[mdbx] Aliased '${_maybe_shared}' -> mdbx::mdbx")
        endif()
    endif()
    if(DEFINED _maybe_static AND NOT "${_maybe_static}" STREQUAL "")
        if(TARGET ${_maybe_static} AND NOT TARGET mdbx::mdbx-static)
            add_library(mdbx::mdbx-static ALIAS ${_maybe_static})
            message(STATUS "[mdbx] Aliased '${_maybe_static}' -> mdbx::mdbx-static")
        endif()
    endif()
endfunction()

function(_mdbx_detect_and_alias out_ok)
    set(${out_ok} FALSE PARENT_SCOPE)

    # common upstream target name variants
    set(_cands_shared
        mdbx::mdbx
        mdbx
        libmdbx::mdbx
        libmdbx
    )
    set(_cands_static
        mdbx::mdbx-static
        mdbx-static
        libmdbx::mdbx-static
        libmdbx-static
        mdbx_static
        libmdbx_static
    )

    set(_found_shared "")
    foreach(t ${_cands_shared})
        if(TARGET ${t})
            set(_found_shared ${t})
            break()
        endif()
    endforeach()

    set(_found_static "")
    foreach(t ${_cands_static})
        if(TARGET ${t})
            set(_found_static ${t})
            break()
        endif()
    endforeach()

    if(_found_shared OR _found_static)
        _mdbx_make_aliases("${_found_shared}" "${_found_static}")
        set(${out_ok} TRUE PARENT_SCOPE)
        return()
    endif()
endfunction()

function(_mdbx_try_find_package out_ok)
    set(${out_ok} FALSE PARENT_SCOPE)

    # Prefer CONFIG
    find_package(MDBX CONFIG QUIET)
    _mdbx_detect_and_alias(_ok1)
    if(MDBX_FOUND OR _ok1)
        if(MDBX_FOUND)
            message(STATUS "[mdbx] Found via find_package(MDBX CONFIG)")
        endif()
        set(${out_ok} TRUE PARENT_SCOPE)
        return()
    endif()

    # Some distros expose MODULE or different casing
    find_package(MDBX QUIET MODULE)
    _mdbx_detect_and_alias(_ok2)
    if(_ok2)
        message(STATUS "[mdbx] Found via find_package(MDBX MODULE)")
        set(${out_ok} TRUE PARENT_SCOPE)
        return()
    endif()

    # Try lowercase package id on some setups
    find_package(mdbx CONFIG QUIET)
    _mdbx_detect_and_alias(_ok3)
    if(mdbx_FOUND OR _ok3)
        if(mdbx_FOUND)
            message(STATUS "[mdbx] Found via find_package(mdbx CONFIG)")
        endif()
        set(${out_ok} TRUE PARENT_SCOPE)
        return()
    endif()
endfunction()

function(_mdbx_try_submodule out_ok)
    set(${out_ok} FALSE PARENT_SCOPE)

    # Expected location of submodule
    set(_MDBX_SRC "${PROJECT_SOURCE_DIR}/libs/libmdbx")
    if(EXISTS "${_MDBX_SRC}/CMakeLists.txt")
        # --- Windows/MSYS quirk: normalize to REALPATH to avoid mixed path issues
        if(WIN32)
            get_filename_component(_MDBX_SRC_REAL "${_MDBX_SRC}" REALPATH)
        else()
            set(_MDBX_SRC_REAL "${_MDBX_SRC}")
        endif()

        # Build options BEFORE add_subdirectory (keep static as before)
        set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
        set(MDBX_BUILD_TOOLS          OFF CACHE BOOL "" FORCE)

        # Optional: allow their install() of static lib in SDK bundle scenarios (kept from your logic)
        if(DEFINED IMGUIX_SDK_BUNDLE_DEPS AND IMGUIX_SDK_BUNDLE_DEPS)
            set(MDBX_INSTALL_STATIC ON CACHE BOOL "" FORCE)
        endif()

        # VERSION.json cleanup to rely on git metadata (as in your original workaround)
        if(EXISTS "${_MDBX_SRC_REAL}/VERSION.json")
            file(REMOVE "${_MDBX_SRC_REAL}/VERSION.json")
            message(STATUS "libmdbx: removed stray VERSION.json (using git metadata)")
        endif()

        # Unique binary dir to avoid clobbering
        add_subdirectory("${_MDBX_SRC_REAL}" "${CMAKE_BINARY_DIR}/_deps/mdbx-build")

        # Upstream usually exposes mdbx-static (or mdbx)
        if(TARGET mdbx-static)
            _mdbx_make_aliases("" "mdbx-static")
            if(NOT TARGET mdbx::mdbx)
                # canonical shared alias points to static in bundled builds
                add_library(mdbx::mdbx ALIAS mdbx-static)
            endif()
        elseif(TARGET mdbx)
            _mdbx_make_aliases("mdbx" "")
        else()
            message(FATAL_ERROR "libmdbx submodule added but no known target (mdbx / mdbx-static) was created")
        endif()

        set(${out_ok} TRUE PARENT_SCOPE)
        return()
    endif()
endfunction()

function(_mdbx_try_fetchcontent out_ok)
    set(${out_ok} FALSE PARENT_SCOPE)

    # Keep STATIC for bundled path to preserve CI behavior
    set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(MDBX_BUILD_TOOLS          OFF CACHE BOOL "" FORCE)
    if(DEFINED IMGUIX_SDK_BUNDLE_DEPS AND IMGUIX_SDK_BUNDLE_DEPS)
        set(MDBX_INSTALL_STATIC ON CACHE BOOL "" FORCE)
    endif()

    FetchContent_Declare(
        libmdbx
        GIT_REPOSITORY https://github.com/erthink/libmdbx.git
        GIT_TAG        stable
        GIT_SHALLOW    TRUE
        FIND_PACKAGE_ARGS
    )
    message(STATUS "[mdbx] Fetching libmdbx (stable)")
    FetchContent_MakeAvailable(libmdbx)

    if(TARGET mdbx-static)
        _mdbx_make_aliases("" "mdbx-static")
        if(NOT TARGET mdbx::mdbx)
            add_library(mdbx::mdbx ALIAS mdbx-static)
        endif()
        set(${out_ok} TRUE PARENT_SCOPE)
        return()
    elseif(TARGET mdbx)
        _mdbx_make_aliases("mdbx" "")
        set(${out_ok} TRUE PARENT_SCOPE)
        return()
    else()
        message(FATAL_ERROR "Unknown libmdbx target names after FetchContent; check upstream CMake")
    endif()
endfunction()

# -------- public API ---------------------------------------------------------

# mdbx_provide(
#     MODE <AUTO|SYSTEM|BUNDLED>
#     OUT_TARGET_STATIC <var>
#     OUT_TARGET_SHARED <var>
# )
function(mdbx_provide)
    set(_oneValueArgs MODE OUT_TARGET_STATIC OUT_TARGET_SHARED)
    cmake_parse_arguments(MX "" "${_oneValueArgs}" "" ${ARGN})

    if(NOT MX_MODE)
        set(MX_MODE "AUTO")
    endif()
    string(TOUPPER "${MX_MODE}" MX_MODE_UP)
    message(STATUS "[mdbx] Provide mode = ${MX_MODE_UP}")

    set(_ok FALSE)

    if(MX_MODE_UP STREQUAL "SYSTEM" OR MX_MODE_UP STREQUAL "AUTO")
        _mdbx_try_find_package(_ok)
    endif()

    if(NOT _ok AND (MX_MODE_UP STREQUAL "BUNDLED" OR MX_MODE_UP STREQUAL "AUTO"))
        _mdbx_try_submodule(_ok)
    endif()

    if(NOT _ok AND (MX_MODE_UP STREQUAL "BUNDLED" OR MX_MODE_UP STREQUAL "AUTO"))
        _mdbx_try_fetchcontent(_ok)
    endif()

    if(NOT _ok)
        if(MX_MODE_UP STREQUAL "SYSTEM")
            message(FATAL_ERROR "[mdbx] SYSTEM mode requested, but no package found.")
        else()
            message(FATAL_ERROR "[mdbx] Failed to provide MDBX (find_package, submodule, FetchContent all failed).")
        endif()
    endif()

    # Exposed aliases should exist by now:
    # - mdbx::mdbx always present (shared if system provided; else alias to static)
    # - mdbx::mdbx-static present if upstream built static target
    if(NOT TARGET mdbx::mdbx)
        message(FATAL_ERROR "[mdbx] Internal error: canonical target mdbx::mdbx was not created")
    endif()

    # Return outputs:
    if(MX_OUT_TARGET_SHARED)
        # may be SHARED (system) or an alias to static (bundled)
        set(${MX_OUT_TARGET_SHARED} "mdbx::mdbx" PARENT_SCOPE)
    endif()

    if(MX_OUT_TARGET_STATIC)
        if(TARGET mdbx::mdbx-static)
            set(${MX_OUT_TARGET_STATIC} "mdbx::mdbx-static" PARENT_SCOPE)
        else()
            # No explicit static target known → fall back to the canonical one
            set(${MX_OUT_TARGET_STATIC} "mdbx::mdbx" PARENT_SCOPE)
        endif()
    endif()
endfunction()
