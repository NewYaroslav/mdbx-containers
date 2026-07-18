# cmake/deps/tiny-process-library.cmake
# Provides tiny-process-library for optional multi-process examples.

include(FetchContent)

set(MDBXC_TINY_PROCESS_LIBRARY_GIT_TAG
    "8bbb5a211c5c9df8ee69301da9d22fb977b27dc1" CACHE STRING
    "tiny-process-library commit used by optional process-spawning examples.")

function(tiny_process_library_provide)
    if(TARGET tiny-process-library::tiny-process-library)
        return()
    endif()

    FetchContent_Declare(
        mdbxc_tiny_process_library
        GIT_REPOSITORY https://gitlab.com/eidheim/tiny-process-library.git
        GIT_TAG        ${MDBXC_TINY_PROCESS_LIBRARY_GIT_TAG}
    )
    FetchContent_GetProperties(mdbxc_tiny_process_library)
    if(NOT mdbxc_tiny_process_library_POPULATED)
        if(COMMAND _mdbxc_fetchcontent_populate)
            _mdbxc_fetchcontent_populate(mdbxc_tiny_process_library)
        else()
            FetchContent_Populate(mdbxc_tiny_process_library)
        endif()
        FetchContent_GetProperties(mdbxc_tiny_process_library)
    endif()

    add_subdirectory(
        "${mdbxc_tiny_process_library_SOURCE_DIR}"
        "${mdbxc_tiny_process_library_BINARY_DIR}"
        EXCLUDE_FROM_ALL)
endfunction()
