#pragma once
#ifndef _MDBX_CONTAINERS_CONFIG_HPP_INCLUDED
#define _MDBX_CONTAINERS_CONFIG_HPP_INCLUDED

/// \file Config.hpp
/// \brief Configuration class for MDBX database.

namespace mdbxc {

    /// \class Config
    /// \brief Configuration for MDBX databases.
    class Config {
    public:
        std::string pathname;                   ///< Pathname for the database or directory in which the database files reside.
        int64_t size_lower  = -1;               ///< Lower bound for database size.
        int64_t size_now    = -1;               ///< Current size of the database.
        int64_t size_upper  = -1;               ///< Upper bound for database size.
        int64_t growth_step = 16 * 1024 * 1024; ///< Step size for database growth.
        int64_t shrink_threshold = 16 * 1024 * 1024; ///< Threshold for database shrinking.
        int64_t page_size   = 0;                ///< Page size; should be a power of two.
        int64_t max_readers = 0;                ///< Maximum number of reader slots; 0 uses the default (twice the number of CPU cores).
        int64_t max_dbs = 10;                   ///< Maximum number of named databases (DBI) within the environment.
        bool read_only = false;                 ///< Enables or disables read-only mode.
        bool readahead = true;                  ///< Enables or disables readahead for sequential data access.
        bool no_subdir = true;      			///< If true, uses a single file instead of a directory for the database.
		bool sync_durable = true;   			///< If true, enforces synchronous/durable writes (MDBX_SYNC_DURABLE).
		bool writemap_mode = false;             ///< Enables or disables the `MDBX_WRITEMAP` mode, which maps the database into memory for direct modification.
        bool relative_to_exe = false;           ///< If true and pathname is relative, interpret it as relative to executable directory.
        
        /// \brief Validate the MDBX configuration.
        /// \return True if the configuration is valid, false otherwise.
        bool validate() const {
            const bool page_ok = (page_size == 0) || ((page_size & (page_size - 1)) == 0);
            const bool size_ok = (size_lower <= size_now || size_now == -1) &&
                                 (size_now <= size_upper || size_now == -1);
            return !pathname.empty() && page_ok && size_ok;
        }
    };

}; // mdbxc

#endif // _MDBX_CONTAINERS_CONFIG_HPP_INCLUDED
