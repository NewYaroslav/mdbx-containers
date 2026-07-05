#pragma once
#ifndef MDBX_CONTAINERS_HEADER_VECTOR_VECTOR_METRIC_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_VECTOR_VECTOR_METRIC_HPP_INCLUDED

namespace mdbxc {
    /// \brief Similarity/distance metric used by vector search.
    enum class VectorMetric {
        COSINE, ///< Cosine similarity; higher score is better.
        DOT,    ///< Raw dot product; higher score is better.
        L2      ///< Negative squared L2 distance; higher score is better.
    };
} // namespace mdbxc

#endif // MDBX_CONTAINERS_HEADER_VECTOR_VECTOR_METRIC_HPP_INCLUDED
