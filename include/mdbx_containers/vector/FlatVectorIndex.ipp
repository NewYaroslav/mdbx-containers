#pragma once
#ifndef _MDBX_CONTAINERS_VECTOR_FLAT_VECTOR_INDEX_IPP_INCLUDED
#define _MDBX_CONTAINERS_VECTOR_FLAT_VECTOR_INDEX_IPP_INCLUDED

#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace mdbxc {

    inline FlatVectorIndex::FlatVectorIndex(VectorMetric metric)
        : m_metric(metric) {}

    inline void FlatVectorIndex::clear() {
        m_ids.clear();
        m_vectors.clear();
        m_dim = 0;
    }

    inline void FlatVectorIndex::check_dim(const Embedding& embedding) {
        embedding.validate();
        if (m_dim == 0) {
            m_dim = embedding.dim;
        } else if (embedding.dim != m_dim) {
            throw std::invalid_argument("Embedding dimension does not match index dimension");
        }
    }

    inline void FlatVectorIndex::add(uint64_t id, const Embedding& embedding) {
        check_dim(embedding);
        std::vector<float> stored(m_dim);
        if (m_metric == VectorMetric::COSINE) {
            float norm = 0.0f;
            for (std::size_t i = 0; i < embedding.values.size(); ++i) {
                norm += embedding.values[i] * embedding.values[i];
            }
            norm = std::sqrt(norm);
            if (norm > 0.0f) {
                for (std::size_t i = 0; i < m_dim; ++i) {
                    stored[i] = embedding.values[i] / norm;
                }
            } else {
                std::fill(stored.begin(), stored.end(), 0.0f);
            }
        } else {
            std::memcpy(stored.data(), embedding.values.data(), m_dim * sizeof(float));
        }
        m_ids.reserve(m_ids.size() + 1);
        m_vectors.reserve(m_vectors.size() + m_dim);
        m_ids.push_back(id);
        m_vectors.insert(m_vectors.end(), stored.begin(), stored.end());
    }

    inline bool FlatVectorIndex::erase(uint64_t id) {
        for (std::size_t i = 0; i < m_ids.size(); ++i) {
            if (m_ids[i] == id) {
                std::size_t last = m_ids.size() - 1;
                if (i != last) {
                    m_ids[i] = m_ids[last];
                    std::memcpy(&m_vectors[i * m_dim], &m_vectors[last * m_dim], m_dim * sizeof(float));
                }
                m_ids.pop_back();
                m_vectors.resize(m_vectors.size() - m_dim);
                if (m_ids.empty()) {
                    m_dim = 0;
                }
                return true;
            }
        }
        return false;
    }

    inline float FlatVectorIndex::compute_score(const float* query_vec,
                                                  const float* candidate_vec) const {
        if (m_metric == VectorMetric::COSINE || m_metric == VectorMetric::DOT) {
            float dot = 0.0f;
            for (std::size_t i = 0; i < m_dim; ++i) {
                dot += query_vec[i] * candidate_vec[i];
            }
            return dot;
        }
        // L2: score = -squared_distance (higher is better)
        float sq_dist = 0.0f;
        for (std::size_t i = 0; i < m_dim; ++i) {
            float diff = query_vec[i] - candidate_vec[i];
            sq_dist += diff * diff;
        }
        return -sq_dist;
    }

    inline std::vector<VectorMatch> FlatVectorIndex::search(const Embedding& query,
                                                              std::size_t top_k) const {
        query.validate();
        if (top_k == 0) {
            return std::vector<VectorMatch>();
        }
        if (m_dim == 0 || m_ids.empty()) {
            return std::vector<VectorMatch>();
        }
        if (query.dim != m_dim) {
            throw std::invalid_argument("Query dimension does not match index dimension");
        }

        // Prepare query vector (normalize for COSINE)
        std::vector<float> query_vec(m_dim);
        if (m_metric == VectorMetric::COSINE) {
            float norm = 0.0f;
            for (std::size_t i = 0; i < query.values.size(); ++i) {
                norm += query.values[i] * query.values[i];
            }
            norm = std::sqrt(norm);
            if (norm > 0.0f) {
                for (std::size_t i = 0; i < m_dim; ++i) {
                    query_vec[i] = query.values[i] / norm;
                }
            } else {
                std::fill(query_vec.begin(), query_vec.end(), 0.0f);
            }
        } else {
            std::memcpy(query_vec.data(), query.values.data(), m_dim * sizeof(float));
        }

        std::size_t n = m_ids.size();
        std::vector<VectorMatch> matches;
        matches.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            VectorMatch m;
            m.id = m_ids[i];
            m.score = compute_score(query_vec.data(), &m_vectors[i * m_dim]);
            matches.push_back(m);
        }

        if (top_k > n) {
            top_k = n;
        }
        std::partial_sort(matches.begin(), matches.begin() + top_k, matches.end(),
            [](const VectorMatch& a, const VectorMatch& b) {
                return a.score > b.score;
            });
        matches.resize(top_k);
        return matches;
    }

    inline std::size_t FlatVectorIndex::size() const noexcept {
        return m_ids.size();
    }

    inline uint32_t FlatVectorIndex::dim() const noexcept {
        return m_dim;
    }

} // namespace mdbxc

#endif // _MDBX_CONTAINERS_VECTOR_FLAT_VECTOR_INDEX_IPP_INCLUDED
