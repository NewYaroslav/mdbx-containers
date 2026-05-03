project(Dependencies LANGUAGES C)

set(EXTERNAL_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(${EXTERNAL_CMAKE_DIR}/install-headers.cmake)
# include(${EXTERNAL_CMAKE_DIR}/install-static-libs.cmake)

# mdbx
include(${EXTERNAL_CMAKE_DIR}/libmdbx-wrapper.cmake)
# move_static_output(mdbx-static)
install_headers_to_include("." ${CMAKE_CURRENT_LIST_DIR}/libmdbx)
