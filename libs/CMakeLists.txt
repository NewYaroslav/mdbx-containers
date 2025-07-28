project(Dependencies LANGUAGES C)

set(LIBS_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(${LIBS_CMAKE_DIR}/install-headers.cmake)
# include(${LIBS_CMAKE_DIR}/install-static-libs.cmake)

# mdbx
include(${LIBS_CMAKE_DIR}/libmdbx-wrapper.cmake)
# move_static_output(mdbx-static)
install_headers_to_include("." ${CMAKE_CURRENT_LIST_DIR}/libmdbx)