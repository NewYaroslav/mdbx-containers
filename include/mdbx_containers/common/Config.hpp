#pragma once
#ifndef MDBX_CONTAINERS_HEADER_COMMON_CONFIG_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_COMMON_CONFIG_HPP_INCLUDED

/// \file Config.hpp
/// \brief Configuration options used when opening an MDBX environment.

#include <limits>

namespace mdbxc {

    /// \class Config
    /// \ingroup mdbxc_core
    /// \brief Parameters used by \ref Connection to create the MDBX environment.
    ///
    /// Each option corresponds to an MDBX flag or setting. See
    /// \ref config_page for a detailed description of how these values
    /// influence the database.
    class Config {
    public:
        std::string pathname;                   ///< Path to the database file or directory containing the database.
        int64_t size_lower  = -1;               ///< Lower bound for database size.
        int64_t size_now    = -1;               ///< Current size of the database.
        int64_t size_upper  = -1;               ///< Upper bound for database size.
        int64_t growth_step = 16 * 1024 * 1024; ///< Step size for database growth.
        int64_t shrink_threshold = 16 * 1024 * 1024; ///< Threshold for database shrinking.
        int64_t page_size   = 0;                ///< Page size (must be 0 for default, or a power of two between 256 and 65536).
        int64_t max_readers = 0;                ///< Maximum reader slots; use 0 for the default (twice the CPU count).
        int64_t max_dbs = 10;                   ///< Maximum number of named databases (DBI) in the environment.
        int64_t max_dupsort_value_size = -1;        ///< Proactive MDBX_DUPSORT duplicate value size limit; <= 0 disables it.
        /// Open the environment with MDBX_RDONLY.
        /// Table wrappers open existing DBIs only in this mode, and missing
        /// directories are not created.
        bool read_only = false;
        bool readahead = true;                  ///< Whether to enable OS readahead for sequential access.
        bool no_subdir = true;                  ///< Whether to store the database in a single file instead of a directory.
        bool sync_durable = true;               ///< Whether to enforce synchronous durable writes (MDBX_SYNC_DURABLE).
        bool writemap_mode = false;             ///< Whether to map the database with MDBX_WRITEMAP for direct modification.
        bool relative_to_exe = false;           ///< Whether to resolve a relative path relative to the executable directory.
        
        /// \brief Validate the MDBX configuration.
        /// \return True if the configuration is valid, false otherwise.
        bool validate() const {
            const bool page_ok =
                (page_size == 0) ||
                (page_size >= 256 && page_size <= 65536 &&
                 (page_size & (page_size - 1)) == 0);
            const bool size_ok =
                (size_lower <= size_now || size_now == -1) &&
                (size_now <= size_upper || size_now == -1);
            const bool readers_ok =
                max_readers >= 0 &&
                max_readers <= static_cast<int64_t>(std::numeric_limits<int>::max());
            const bool dbs_ok = max_dbs >= 1;
            return !pathname.empty() && page_ok && size_ok && readers_ok && dbs_ok;
        }
    };

}; // mdbxc

#endif // MDBX_CONTAINERS_HEADER_COMMON_CONFIG_HPP_INCLUDED
