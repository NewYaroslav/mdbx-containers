#include <cassert>
#include <iostream>
#include <map>
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
        mdbxc::KeyMultiValueTable<int, std::string> table(conn, "manual_multi_values");
        table.clear();

        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        table.insert(1, "one", txn);
        table.insert(1, "one", txn);
        txn.commit();

        auto read_txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        assert(table.count(1, read_txn) == 2);
        assert(table.count(1, std::string("one"), read_txn) == 2);
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

    std::cout << "KeyMultiValueTable test passed.\n";
    return 0;
}
