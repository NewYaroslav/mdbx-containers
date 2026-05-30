#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <mdbx_containers/KeyMultiValueTable.hpp>

namespace {

template <class T>
void assert_vector_equal(const std::vector<T>& actual, const std::vector<T>& expected) {
    assert(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        assert(actual[i] == expected[i]);
    }
}

template<class Fn>
void assert_throws_length_error(Fn fn) {
    bool thrown = false;
    try {
        fn();
    } catch (const std::length_error&) {
        thrown = true;
    }
    assert(thrown);
}

} // namespace

int main() {
    mdbxc::Config cfg;
    cfg.pathname = "data/key_multi_value_table_test.mdbx";
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    cfg.relative_to_exe = true;

    auto conn = mdbxc::Connection::create(cfg);

    {
        mdbxc::KeyMultiValueTable<int, std::string> table(conn, "multi_values");
        table.clear();

        assert(table.empty());
        table.insert(7, "created");
        table.insert(7, "created");
        table.insert(7, "sent");
        table.insert(8, "queued");

        assert(table.count() == 4);
        assert(table.count(7) == 3);
        assert(table.count(7, std::string("created")) == 2);
        assert(table.count(7, std::string("missing")) == 0);
        assert(table.contains(7));
        assert(table.contains(7, std::string("sent")));
        assert(!table.contains(9));

        assert_vector_equal(table.find(7), std::vector<std::string>{"created", "created", "sent"});
        std::vector<std::pair<int, std::string> > range_pairs;
        range_pairs.push_back(std::make_pair(7, std::string("created")));
        range_pairs.push_back(std::make_pair(7, std::string("created")));
        range_pairs.push_back(std::make_pair(7, std::string("sent")));
        range_pairs.push_back(std::make_pair(8, std::string("queued")));
        std::multimap<int, std::string> range_multimap = table.range(7, 8);
        assert(range_multimap.size() == 4);
        assert(range_multimap.count(7) == 3);
        assert(range_multimap.count(8) == 1);
        assert_vector_equal(table.range_vector(7, 8), range_pairs);
        assert_vector_equal(table.range_values(7, 8),
                            std::vector<std::string>{"created", "created", "sent", "queued"});
        assert(table.range_values<std::set>(7, 8) ==
               (std::set<std::string>{"created", "queued", "sent"}));

        std::vector<std::pair<int, std::string> > queued_pair;
        queued_pair.push_back(std::make_pair(8, std::string("queued")));
        assert(table.range(8, 8).size() == 1);
        assert_vector_equal(table.range_vector(8, 8), queued_pair);
        assert_vector_equal(table.range_values(8, 8), std::vector<std::string>{"queued"});
        assert(table.range(9, 10).empty());
        assert(table.range(8, 7).empty());

        std::multimap<int, std::string> as_multimap;
        table.load(as_multimap);
        assert(as_multimap.size() == 4);
        assert(as_multimap.count(7) == 3);

        std::vector<std::pair<int, std::string> > as_vector;
        table.load(as_vector);
        assert(as_vector.size() == 4);
        assert(as_vector[0] == std::make_pair(7, std::string("created")));
        assert(as_vector[1] == std::make_pair(7, std::string("created")));
        assert(as_vector[2] == std::make_pair(7, std::string("sent")));
        assert(as_vector[3] == std::make_pair(8, std::string("queued")));

        assert(table.erase(7, std::string("created")) == 2);
        assert_vector_equal(table.find(7), std::vector<std::string>{"sent"});
        assert(table.erase(7));
        assert(!table.contains(7));
        assert(table.count() == 1);

        std::vector<std::pair<int, std::string> > replacement;
        replacement.push_back(std::make_pair(3, std::string("a")));
        replacement.push_back(std::make_pair(3, std::string("a")));
        replacement.push_back(std::make_pair(3, std::string("b")));
        table.reconcile(replacement);
        assert(table.count() == 3);
        assert(table.count(3, std::string("a")) == 2);
        assert_vector_equal(table.find(3), std::vector<std::string>{"a", "a", "b"});

        std::multimap<int, std::string> assigned;
        assigned.emplace(4, "x");
        assigned.emplace(4, "x");
        table = assigned;
        assert(table.count() == 2);
        assert(table.count(4, std::string("x")) == 2);
        assert(!table.contains(3));
    }

    {
        mdbxc::KeyMultiValueTable<int, std::string> table(conn, "multi_key_range");
        table.clear();

        table.insert(1, "a");
        table.insert(1, "b");
        table.insert(2, "c");
        table.insert(2, "d");
        table.insert(3, "e");

        std::vector<std::pair<int, std::string> > expected_pairs;
        expected_pairs.push_back(std::make_pair(1, std::string("a")));
        expected_pairs.push_back(std::make_pair(1, std::string("b")));
        expected_pairs.push_back(std::make_pair(2, std::string("c")));
        expected_pairs.push_back(std::make_pair(2, std::string("d")));
        assert(table.range(1, 2).size() == 4);
        assert_vector_equal(table.range_vector(1, 2), expected_pairs);
        assert_vector_equal(table.range_values(1, 2),
                            std::vector<std::string>{"a", "b", "c", "d"});
    }

    {
        mdbxc::KeyMultiValueTable<int, std::string> table(conn, "manual_multi_values");
        table.clear();

        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        table.insert(1, "one", txn);
        table.insert(1, "one", txn);
        txn.commit();

        auto read_txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        assert(table.count(1, read_txn) == 2);
        assert(table.count(1, std::string("one"), read_txn) == 2);
        std::vector<std::pair<int, std::string> > one_pairs;
        one_pairs.push_back(std::make_pair(1, std::string("one")));
        one_pairs.push_back(std::make_pair(1, std::string("one")));
        assert(table.range(1, 1, read_txn).size() == 2);
        assert_vector_equal(table.range_vector(1, 1, read_txn), one_pairs);
        assert_vector_equal(table.range_values(1, 1, read_txn), std::vector<std::string>{"one", "one"});
        read_txn.commit();
    }

    {
        mdbxc::KeyMultiValueTable<int, std::string> table(conn, "incremental_reconcile");
        table.clear();

        table.insert(1, "a");
        table.insert(1, "b");
        table.insert(1, "a");
        table.insert(2, "old");

        std::vector<std::pair<int, std::string> > same_multiset_reordered;
        same_multiset_reordered.push_back(std::make_pair(1, std::string("b")));
        same_multiset_reordered.push_back(std::make_pair(1, std::string("a")));
        same_multiset_reordered.push_back(std::make_pair(1, std::string("a")));
        same_multiset_reordered.push_back(std::make_pair(2, std::string("old")));
        table.reconcile(same_multiset_reordered);
        assert(table.count() == 4);
        assert_vector_equal(table.find(1), std::vector<std::string>{"a", "b", "a"});

        std::vector<std::pair<int, std::string> > fewer_duplicates;
        fewer_duplicates.push_back(std::make_pair(1, std::string("a")));
        table.reconcile(fewer_duplicates);
        assert(table.count() == 1);
        assert(table.count(1, std::string("a")) == 1);
        assert(!table.contains(2));
        assert_vector_equal(table.find(1), std::vector<std::string>{"a"});

        std::vector<std::pair<int, std::string> > more_and_changed;
        more_and_changed.push_back(std::make_pair(1, std::string("a")));
        more_and_changed.push_back(std::make_pair(1, std::string("a")));
        more_and_changed.push_back(std::make_pair(1, std::string("c")));
        more_and_changed.push_back(std::make_pair(3, std::string("z")));
        table.reconcile(more_and_changed);
        assert(table.count() == 4);
        assert(table.count(1, std::string("a")) == 2);
        assert(table.count(1, std::string("c")) == 1);
        assert(table.count(3, std::string("z")) == 1);
        assert_vector_equal(table.find(1), std::vector<std::string>{"a", "a", "c"});

        std::multimap<int, std::string> multimap_source;
        multimap_source.insert(std::make_pair(4, std::string("x")));
        multimap_source.insert(std::make_pair(4, std::string("x")));
        multimap_source.insert(std::make_pair(4, std::string("y")));
        table.reconcile(multimap_source);
        assert(table.count() == 3);
        assert(table.count(4, std::string("x")) == 2);
        assert(table.count(4, std::string("y")) == 1);
        assert(!table.contains(1));

        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        std::vector<std::pair<int, std::string> > txn_source;
        txn_source.push_back(std::make_pair(5, std::string("txn")));
        txn_source.push_back(std::make_pair(5, std::string("txn")));
        table.reconcile(txn_source, txn);
        txn.commit();
        assert(table.count() == 2);
        assert(table.count(5, std::string("txn")) == 2);
        assert(!table.contains(4));
    }

    {
        mdbxc::KeyMultiValueTable<int, std::string> safe_table(conn, "multi_options");
        mdbxc::KeyMultiValueTable<int, std::string, mdbxc::FastIntegerKeyOptions> fast_table(conn, "multi_options");
        safe_table.clear();
        safe_table.insert(11, "safe");
        fast_table.insert(12, "fast");
        assert(fast_table.count(11, std::string("safe")) == 1);
        assert(safe_table.count(12, std::string("fast")) == 1);
    }

    {
        mdbxc::Config limit_cfg;
        limit_cfg.pathname = "data/key_multi_value_oversized_test.mdbx";
        limit_cfg.max_dbs = 4;
        limit_cfg.max_dupsort_value_size = 128;
        limit_cfg.no_subdir = true;
        limit_cfg.relative_to_exe = true;

        auto limit_conn = mdbxc::Connection::create(limit_cfg);
        mdbxc::KeyMultiValueTable<int, std::string> table(limit_conn, "multi_oversized");
        table.clear();
        std::string large(1024, 'x');
        assert_throws_length_error([&table, &large]() {
            table.insert(1, large);
        });
    }

    // --- Range API extension tests ---
    {
        mdbxc::KeyMultiValueTable<int, std::string> table(conn, "multi_range_api");
        table.clear();
        table.insert(1, "a");
        table.insert(1, "b");
        table.insert(2, "c");
        table.insert(3, "d");
        table.insert(3, "e");
        table.insert(3, "f");

        // for_each_range visits every physical pair
        std::vector<std::pair<int, std::string>> collected;
        bool completed = table.for_each_range(1, 3, [&collected](const int& k, const std::string& v) -> bool {
            collected.push_back(std::make_pair(k, v));
            return true;
        });
        assert(completed);
        assert(collected.size() == 6);

        // filter_range
        std::vector<std::pair<int, std::string>> filtered = table.filter_range(1, 3, [](const int&, const std::string& v) -> bool {
            return v >= "d";
        });
        assert(filtered.size() == 3); // d, e, f

        // reverse range
        std::vector<std::pair<int, std::string>> rev = table.range_reverse(1, 3);
        assert(rev.size() == 6);
        assert(rev[0] == std::make_pair(3, std::string("f")));
        assert(rev[5] == std::make_pair(1, std::string("a")));

        // reverse range limit
        std::vector<std::pair<int, std::string>> rev_limit = table.range_reverse(1, 3, 2);
        assert(rev_limit.size() == 2);
        assert(rev_limit[0] == std::make_pair(3, std::string("f")));
        assert(rev_limit[1] == std::make_pair(3, std::string("e")));

        // contains_range / count_range / erase_range
        assert(table.contains_range(2, 3));
        assert(table.count_range(2, 3) == 4); // c + d,e,f
        std::size_t erased = table.erase_range(2, 3);
        assert(erased == 4);
        assert(table.count() == 2);

    }

#if __cplusplus >= 201703L
    {
        mdbxc::KeyMultiValueTable<int, std::string> table(conn, "multi_range_api_opt");
        table.clear();
        table.insert(10, "x");
        table.insert(10, "z");
        table.insert(20, "y");

        auto lb = table.lower_bound(10);
        if (!lb.has_value() || lb->first != 10 || lb->second != "x") {
            throw std::runtime_error("lower_bound failed for KeyMultiValueTable");
        }
        auto ub = table.upper_bound(10);
        if (!ub.has_value() || ub->first != 20 || ub->second != "y") {
            throw std::runtime_error("upper_bound skipped to duplicate instead of next key");
        }
        auto fr = table.first();
        if (!fr.has_value() || fr->first != 10) {
            throw std::runtime_error("first failed for KeyMultiValueTable");
        }
        auto la = table.last();
        if (!la.has_value() || la->first != 20) {
            throw std::runtime_error("last failed for KeyMultiValueTable");
        }
        auto min_key = table.min_key();
        if (!min_key.has_value() || min_key.value() != 10) {
            throw std::runtime_error("min_key failed for KeyMultiValueTable");
        }
        auto max_key = table.max_key();
        if (!max_key.has_value() || max_key.value() != 20) {
            throw std::runtime_error("max_key failed for KeyMultiValueTable");
        }
    }
#endif

#if __cplusplus < 201703L
    {
        mdbxc::KeyMultiValueTable<int, std::string> bounds_table(conn, "multi_range_api_bounds");
        bounds_table.clear();
        bounds_table.insert(1, "a");
        bounds_table.insert(1, "b");
        bounds_table.insert(2, "c");
        bounds_table.insert(3, "d");
        std::pair<bool, std::pair<int, std::string>> lb = bounds_table.lower_bound_compat(1);
        if (!lb.first || lb.second != std::make_pair(1, std::string("a"))) {
            throw std::runtime_error("lower_bound_compat failed for KeyMultiValueTable");
        }
        std::pair<bool, std::pair<int, std::string>> ub = bounds_table.upper_bound_compat(1);
        if (!ub.first || ub.second != std::make_pair(2, std::string("c"))) {
            throw std::runtime_error("upper_bound_compat skipped to duplicate instead of next key");
        }
        std::pair<bool, std::pair<int, std::string>> named_ub = bounds_table.upper_bound(1);
        if (!named_ub.first || named_ub.second != std::make_pair(2, std::string("c"))) {
            throw std::runtime_error("upper_bound skipped to duplicate instead of next key in C++11");
        }
        std::pair<bool, int> min_key = bounds_table.min_key_compat();
        if (!min_key.first || min_key.second != 1) {
            throw std::runtime_error("min_key_compat failed for KeyMultiValueTable");
        }
        std::pair<bool, int> max_key = bounds_table.max_key_compat();
        if (!max_key.first || max_key.second != 3) {
            throw std::runtime_error("max_key_compat failed for KeyMultiValueTable");
        }
        std::pair<bool, int> named_max_key = bounds_table.max_key();
        if (!named_max_key.first || named_max_key.second != 3) {
            throw std::runtime_error("max_key failed for KeyMultiValueTable C++11");
        }
    }
#endif

    std::cout << "KeyMultiValueTable test passed.\n";
    return 0;
}
