#pragma once
#ifndef MDBX_CONTAINERS_HEADER_VECTOR_VECTOR_RECORD_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_VECTOR_VECTOR_RECORD_HPP_INCLUDED

#include "Embedding.hpp"
#include <string>
#include <cstdint>

namespace mdbxc {

    /// \brief Complete logical vector record.
    ///
    /// \note The MVP \ref VectorStore persists this record across separate
    /// tables rather than serializing \c VectorRecord as one blob.
    struct VectorRecord {
        uint64_t id = 0; ///< Stable record id.
        std::string collection; ///< Collection name.
        Embedding embedding; ///< Dense embedding.
        std::string text; ///< Caller payload text.
        std::string metadata_json; ///< Caller metadata encoded as JSON text.
    };

} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_VECTOR_VECTOR_RECORD_HPP_INCLUDED
