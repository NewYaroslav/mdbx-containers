#pragma once
#ifndef MDBX_CONTAINERS_HEADER_EXAMPLES_SYNC_EXAMPLE_UTILS_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_EXAMPLES_SYNC_EXAMPLE_UTILS_HPP_INCLUDED

#include <mdbx_containers.hpp>
#include <mdbx_containers/sync.hpp>

#include <cstdio>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace sync_example {

/// \brief Builds a deterministic demo-only 16-byte id from \p seed.
/// \details The seed values in examples have no protocol meaning. They only
/// make distinct NodeId/DbId values easy to see in small programs. Production
/// code should generate and persist real identifiers instead.
inline mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId n{};
    for (int i = 0; i < 16; ++i) {
        n[i] = static_cast<std::uint8_t>(seed + i);
    }
    return n;
}

inline mdbxc::Config config(const std::string& path,
                            std::size_t max_dbs = 32) {
    mdbxc::Config c;
    c.pathname = path;
    c.max_dbs = max_dbs;
    // Keep every example as one .mdbx file plus one .mdbx-lck file in the
    // current directory, instead of creating a directory per environment.
    c.no_subdir = true;
    return c;
}

inline void cleanup(const std::string& path) {
    // MDBX in no_subdir mode uses the data file and a sibling lock file.
    // Remove both so rerunning an example starts from an empty database.
    std::remove(path.c_str());
    std::remove((path + "-lck").c_str());
}

inline std::shared_ptr<mdbxc::Connection> open(const std::string& path,
                                               std::size_t max_dbs = 32) {
    // Connection::create(config) constructs and opens the MDBX environment.
    return mdbxc::Connection::create(config(path, max_dbs));
}

inline void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class KVT>
typename KVT::value_type::second_type kv_or_throw(
        const std::shared_ptr<mdbxc::Connection>& conn,
        KVT& table,
        const typename KVT::value_type::first_type& key,
        const std::string& what) {
    typename KVT::value_type::second_type out{};
    // Use a short read-only transaction for each inspection so examples do
    // not keep a read transaction open across sync operations.
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    if (!table.try_get(key, out, txn.handle())) {
        throw std::runtime_error("missing: " + what);
    }
    return out;
}

template<class KVT>
bool kv_has(const std::shared_ptr<mdbxc::Connection>& conn,
            KVT& table,
            const typename KVT::value_type::first_type& key) {
    typename KVT::value_type::second_type out{};
    // Same short-lived read-only transaction pattern as kv_or_throw().
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    return table.try_get(key, out, txn.handle());
}

template<class ValueT>
ValueT sequence_or_throw(const std::shared_ptr<mdbxc::Connection>& conn,
                         mdbxc::SequenceTable<ValueT>& table,
                         std::uint64_t id,
                         const std::string& what) {
    ValueT out{};
    // Same short-lived read-only transaction pattern as kv_or_throw().
    auto txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    if (!table.try_get(id, out, txn.handle())) {
        throw std::runtime_error("missing: " + what);
    }
    return out;
}

inline void disconnect_and_cleanup(std::shared_ptr<mdbxc::Connection>& conn,
                                   const std::string& path) {
    if (conn) {
        // Close before deleting files. On Windows an open MDBX handle keeps
        // the data/lock files from being removed.
        conn->disconnect();
        conn.reset();
    }
    cleanup(path);
}

} // namespace sync_example

#endif // MDBX_CONTAINERS_HEADER_EXAMPLES_SYNC_EXAMPLE_UTILS_HPP_INCLUDED
