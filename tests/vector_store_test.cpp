#include "test_assert.hpp"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <mdbx_containers/vector.hpp>

static mdbxc::Embedding make_embedding(const std::vector<float>& vals) {
    mdbxc::Embedding e;
    e.dim = static_cast<uint32_t>(vals.size());
    e.values = vals;
    return e;
}

int main() {
    // --- 1. Add and search ---
    {
        mdbxc::Config cfg;
        cfg.pathname = "data/vector_store_test_1.mdbx";
        cfg.max_dbs = 10;
        cfg.no_subdir = true;
        cfg.relative_to_exe = true;

        mdbxc::VectorStore store(cfg, "test1");
        store.clear();

        mdbxc::Embedding e1 = make_embedding({1.0f, 0.0f, 0.0f});
        mdbxc::Embedding e2 = make_embedding({0.0f, 1.0f, 0.0f});
        store.add(e1, "A", "{\"source\":\"unit\"}");
        store.add(e2, "B");

        mdbxc::Embedding query = make_embedding({1.0f, 0.0f, 0.0f});
        std::vector<mdbxc::SearchResult> results = store.search(query, 1);
        MDBXC_TEST_ASSERT(results.size() == 1);
        MDBXC_TEST_ASSERT(results[0].text == "A");
        MDBXC_TEST_ASSERT(results[0].metadata_json == "{\"source\":\"unit\"}");
    }

    // --- 2. Top-k order ---
    {
        mdbxc::Config cfg;
        cfg.pathname = "data/vector_store_test_2.mdbx";
        cfg.max_dbs = 10;
        cfg.no_subdir = true;
        cfg.relative_to_exe = true;

        mdbxc::VectorStore store(cfg, "test2");
        store.clear();

        mdbxc::Embedding e1 = make_embedding({1.0f, 0.0f});
        mdbxc::Embedding e2 = make_embedding({0.8f, 0.2f});
        mdbxc::Embedding e3 = make_embedding({0.0f, 1.0f});
        uint64_t id1 = store.add(e1, "first");
        store.add(e2, "second");
        store.add(e3, "third");

        mdbxc::Embedding query = make_embedding({1.0f, 0.0f});
        std::vector<mdbxc::SearchResult> results = store.search(query, 2);
        MDBXC_TEST_ASSERT(results.size() == 2);
        MDBXC_TEST_ASSERT(results[0].id == id1);
    }

    // --- 3. Persistence rebuild ---
    {
        mdbxc::Config cfg;
        cfg.pathname = "data/vector_store_test_3.mdbx";
        cfg.max_dbs = 10;
        cfg.no_subdir = true;
        cfg.relative_to_exe = true;

        {
            mdbxc::VectorStore store(cfg, "persist");
            store.clear();
            mdbxc::Embedding e1 = make_embedding({1.0f, 0.0f});
            store.add(e1, "persisted");
        }

        {
            mdbxc::VectorStore store(cfg, "persist");
            mdbxc::Embedding query = make_embedding({1.0f, 0.0f});
            std::vector<mdbxc::SearchResult> results = store.search(query, 1);
            MDBXC_TEST_ASSERT(results.size() == 1);
            MDBXC_TEST_ASSERT(results[0].text == "persisted");
        }
    }

    // --- 4. Erase ---
    {
        mdbxc::Config cfg;
        cfg.pathname = "data/vector_store_test_4.mdbx";
        cfg.max_dbs = 10;
        cfg.no_subdir = true;
        cfg.relative_to_exe = true;

        mdbxc::VectorStore store(cfg, "erase_test");
        store.clear();

        mdbxc::Embedding e1 = make_embedding({1.0f, 0.0f});
        mdbxc::Embedding e2 = make_embedding({0.0f, 1.0f});
        store.add(e1, "keep");
        uint64_t id2 = store.add(e2, "remove");

        MDBXC_TEST_ASSERT(store.erase(id2));

        mdbxc::Embedding query = make_embedding({0.0f, 1.0f});
        std::vector<mdbxc::SearchResult> results = store.search(query, 10);
        for (std::size_t i = 0; i < results.size(); ++i) {
            MDBXC_TEST_ASSERT(results[i].id != id2);
        }
    }

    // --- 5. Dimension mismatch ---
    {
        mdbxc::Config cfg;
        cfg.pathname = "data/vector_store_test_5.mdbx";
        cfg.max_dbs = 10;
        cfg.no_subdir = true;
        cfg.relative_to_exe = true;

        mdbxc::VectorStore store(cfg, "dim_test");
        store.clear();

        mdbxc::Embedding e3 = make_embedding({1.0f, 0.0f, 0.0f});
        store.add(e3, "dim3");

        mdbxc::Embedding query2 = make_embedding({1.0f, 0.0f});
        bool threw = false;
        try {
            store.search(query2, 1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        MDBXC_TEST_ASSERT(threw);
    }

    // --- 6. Empty embedding ---
    {
        mdbxc::Config cfg;
        cfg.pathname = "data/vector_store_test_6.mdbx";
        cfg.max_dbs = 10;
        cfg.no_subdir = true;
        cfg.relative_to_exe = true;

        mdbxc::VectorStore store(cfg, "empty_test");
        store.clear();

        mdbxc::Embedding empty_e;
        bool threw = false;
        try {
            store.add(empty_e, "bad");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        MDBXC_TEST_ASSERT(threw);
    }

    // --- 7. Collections isolation ---
    {
        mdbxc::Config cfg;
        cfg.pathname = "data/vector_store_test_7.mdbx";
        cfg.max_dbs = 20;
        cfg.no_subdir = true;
        cfg.relative_to_exe = true;

        auto conn = mdbxc::Connection::create(cfg);

        mdbxc::VectorStore storeA(conn, "docs");
        mdbxc::VectorStore storeB(conn, "code/raw");
        storeA.clear();
        storeB.clear();
        MDBXC_TEST_ASSERT(storeB.collection() == "code_raw");

        mdbxc::Embedding e1 = make_embedding({1.0f, 0.0f});
        storeA.add(e1, "document");

        mdbxc::Embedding query = make_embedding({1.0f, 0.0f});
        std::vector<mdbxc::SearchResult> resultsB = storeB.search(query, 1);
        MDBXC_TEST_ASSERT(resultsB.empty());
    }

    // --- 8. FlatVectorIndex metrics and top_k=0 ---
    {
        mdbxc::FlatVectorIndex dot_index(mdbxc::VectorMetric::DOT);
        dot_index.add(10, make_embedding({2.0f, 0.0f}));
        dot_index.add(11, make_embedding({0.0f, 1.0f}));
        std::vector<mdbxc::VectorMatch> dot_results =
            dot_index.search(make_embedding({1.0f, 0.0f}), 1);
        MDBXC_TEST_ASSERT(dot_results.size() == 1);
        MDBXC_TEST_ASSERT(dot_results[0].id == 10);
        MDBXC_TEST_ASSERT(dot_results[0].score == 2.0f);
        MDBXC_TEST_ASSERT(dot_index.search(make_embedding({1.0f, 0.0f}), 0).empty());

        mdbxc::FlatVectorIndex l2_index(mdbxc::VectorMetric::L2);
        l2_index.add(20, make_embedding({1.0f, 0.0f}));
        l2_index.add(21, make_embedding({3.0f, 0.0f}));
        std::vector<mdbxc::VectorMatch> l2_results =
            l2_index.search(make_embedding({2.5f, 0.0f}), 2);
        MDBXC_TEST_ASSERT(l2_results.size() == 2);
        MDBXC_TEST_ASSERT(l2_results[0].id == 21);
        MDBXC_TEST_ASSERT(l2_index.erase(20));
        MDBXC_TEST_ASSERT(l2_index.erase(21));
        MDBXC_TEST_ASSERT(l2_index.dim() == 0);
    }

    // --- 9. Embedding serialization validation ---
    {
        mdbxc::Embedding embedding = make_embedding({1.0f, 2.0f});
        std::vector<uint8_t> bytes = embedding.to_bytes();
        mdbxc::Embedding restored = mdbxc::Embedding::from_bytes(bytes.data(), bytes.size());
        MDBXC_TEST_ASSERT(restored.dim == 2);
        MDBXC_TEST_ASSERT(restored.values == embedding.values);

        bool threw = false;
        try {
            mdbxc::Embedding invalid;
            invalid.dim = 3;
            invalid.values.push_back(1.0f);
            invalid.to_bytes();
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        MDBXC_TEST_ASSERT(threw);

        threw = false;
        bytes[4] = 1;
        try {
            mdbxc::Embedding::from_bytes(bytes.data(), bytes.size());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        MDBXC_TEST_ASSERT(threw);
    }

    std::cout << "VectorStore test passed.\n";
    return 0;
}
