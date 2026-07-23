#include <mdbx_containers.hpp>

#include "test_assert.hpp"

#include <cstdint>
#include <string>

#ifndef MDBXC_SYNC_ENABLED
#error "MDBXC_SYNC_ENABLED must be defined by the test target"
#endif

#if !MDBXC_SYNC_ENABLED
#error "header_all_umbrella_test must be compiled with sync enabled"
#endif

int main() {
    mdbxc::Config config;
    config.max_dbs = 8;
    MDBXC_TEST_ASSERT(config.max_dbs == 8);

    mdbxc::KeyValueTable<std::uint64_t, std::string>* key_value_table = nullptr;
    MDBXC_TEST_ASSERT(key_value_table == nullptr);

    mdbxc::KeyOrderedMultiValueTable<std::uint64_t, std::string>* ordered_multi_value_table = nullptr;
    MDBXC_TEST_ASSERT(ordered_multi_value_table == nullptr);

    mdbxc::sync::NodeId node = mdbxc::sync::make_zero_node();
    MDBXC_TEST_ASSERT(node.size() == 16u);

    mdbxc::sync::ChangeBatch batch;
    batch.origin_node_id = node;
    batch.seq = 1;
    MDBXC_TEST_ASSERT(batch.seq == 1u);

    mdbxc::Embedding embedding;
    embedding.dim = 2;
    embedding.values.push_back(1.0f);
    embedding.values.push_back(0.0f);
    MDBXC_TEST_ASSERT(embedding.values.size() == 2u);

    mdbxc::VectorStore* store = nullptr;
    MDBXC_TEST_ASSERT(store == nullptr);

    return 0;
}
