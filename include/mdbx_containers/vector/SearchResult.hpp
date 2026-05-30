#pragma once
#ifndef _MDBX_CONTAINERS_VECTOR_SEARCH_RESULT_HPP_INCLUDED
#define _MDBX_CONTAINERS_VECTOR_SEARCH_RESULT_HPP_INCLUDED

#include <string>
#include <cstdint>

namespace mdbxc {

    /// \brief Result row returned by \ref VectorStore::search().
    struct SearchResult {
        uint64_t id = 0; ///< Matched record id.
        float score = 0.0f; ///< Metric score; larger values rank first.
        std::string collection; ///< Collection that produced the match.
        std::string text; ///< Stored text payload.
        std::string metadata_json; ///< Stored metadata JSON text.
    };

} // namespace mdbxc

#endif // _MDBX_CONTAINERS_VECTOR_SEARCH_RESULT_HPP_INCLUDED
