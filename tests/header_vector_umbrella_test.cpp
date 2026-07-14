#include <mdbx_containers/vector.hpp>

#include "test_assert.hpp"

#include <cstdint>
#include <string>
#include <vector>

int main() {
    const mdbxc::VectorMetric metric = mdbxc::VectorMetric::COSINE;
    MDBXC_TEST_ASSERT(metric == mdbxc::VectorMetric::COSINE);

    mdbxc::Embedding embedding;
    embedding.dim = 2;
    embedding.values.push_back(1.0f);
    embedding.values.push_back(0.0f);
    const std::vector<std::uint8_t> embedding_bytes = embedding.to_bytes();
    const mdbxc::Embedding restored =
        mdbxc::Embedding::from_bytes(embedding_bytes.data(), embedding_bytes.size());
    MDBXC_TEST_ASSERT(restored.dim == 2u);

    mdbxc::VectorRecord record;
    record.id = 7;
    record.collection = "headers";
    record.embedding = restored;
    MDBXC_TEST_ASSERT(record.embedding.values.size() == 2u);

    mdbxc::SearchResult result;
    result.id = record.id;
    result.collection = record.collection;
    MDBXC_TEST_ASSERT(result.id == 7u);

    mdbxc::VectorStore* store = nullptr;
    MDBXC_TEST_ASSERT(store == nullptr);

    return 0;
}
