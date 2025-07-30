/* This is CMake-template for libmdbx's version.c
 ******************************************************************************/

#include "internals.h"

#if !defined(MDBX_VERSION_UNSTABLE) &&                                                                                 \
    (MDBX_VERSION_MAJOR != 0 || MDBX_VERSION_MINOR != 14)
#error "API version mismatch! Had `git fetch --tags` done?"
#endif

static const char sourcery[] =
#ifdef MDBX_VERSION_UNSTABLE
    "UNSTABLE@"
#endif
    MDBX_STRINGIFY(MDBX_BUILD_SOURCERY);

__dll_export
#ifdef __attribute_used__
    __attribute_used__
#elif defined(__GNUC__) || __has_attribute(__used__)
    __attribute__((__used__))
#endif
#ifdef __attribute_externally_visible__
        __attribute_externally_visible__
#elif (defined(__GNUC__) && !defined(__clang__)) || __has_attribute(__externally_visible__)
    __attribute__((__externally_visible__))
#endif
    const struct MDBX_version_info mdbx_version = {
        0,
        14,
        1,
        0,
        "", /* pre-release suffix of SemVer
                                        0.14.1 */
        {"2025-05-06T14:15:36+03:00", "1c8f0e50d4b62e8e5a881a86b049d6a3e17a3edd", "a13147d115ff87e76046d019af5a60b42f4ad323", "v0.14.1-0-ga13147d1"},
        sourcery};

__dll_export
#ifdef __attribute_used__
    __attribute_used__
#elif defined(__GNUC__) || __has_attribute(__used__)
    __attribute__((__used__))
#endif
#ifdef __attribute_externally_visible__
        __attribute_externally_visible__
#elif (defined(__GNUC__) && !defined(__clang__)) || __has_attribute(__externally_visible__)
    __attribute__((__externally_visible__))
#endif
    const char *const mdbx_sourcery_anchor = sourcery;
