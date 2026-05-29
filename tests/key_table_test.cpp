#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <mdbx_containers/KeyTable.hpp>

int main() {
    mdbxc::Config cfg;
    cfg.pathname = "data/key_table_test.mdbx";
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    cfg.relative_to_exe = true;

    auto conn = mdbxc::Connection::create(cfg);

    {
        mdbxc::KeyTable<std::string> table(conn, "keys");
        table.clear();

        assert(table.empty());
        assert(table.insert("alpha"));
        assert(!table.insert("alpha"));
        assert(table.insert("beta"));
        assert(table.contains("alpha"));
        assert(!table.contains("gamma"));
        assert(table.count() == 2);

        std::set<std::string> as_set;
        table.load(as_set);
        assert(as_set == (std::set<std::string>{"alpha", "beta"}));

        std::vector<std::string> as_vector = table.retrieve_all<std::vector>();
        assert(as_vector.size() == 2);

        assert(table.range("alpha", "beta") ==
               (std::vector<std::string>{"alpha", "beta"}));
        assert(table.range("aardvark", "alpha") ==
               (std::vector<std::string>{"alpha"}));
        assert(table.range("gamma", "omega").empty());
        assert(table.range("zeta", "alpha").empty());

        assert(table.erase("alpha"));
        assert(!table.erase("alpha"));
        assert(!table.contains("alpha"));
        assert(table.count() == 1);

        std::set<std::string> replacement{"delta", "epsilon"};
        table.reconcile(replacement);
        assert(table.count() == 2);
        assert(table.contains("delta"));
        assert(!table.contains("beta"));

        std::set<std::string> assigned{"zeta"};
        table = assigned;
        assert(table.count() == 1);
        assert(table.contains("zeta"));
    }

    {
        mdbxc::KeyTable<int> table(conn, "manual_keys");
        table.clear();

        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        assert(table.insert(1, txn));
        assert(table.insert(2, txn));
        txn.commit();

        auto read_txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        assert(table.contains(1, read_txn));
        assert(table.count(read_txn) == 2);
        assert(table.range(1, 2, read_txn) == (std::vector<int>{1, 2}));
        read_txn.commit();
    }

    {
        mdbxc::KeyTable<int> safe_table(conn, "key_options");
        mdbxc::KeyTable<int, mdbxc::FastIntegerKeyOptions> fast_table(conn, "key_options");
        safe_table.clear();
        assert(safe_table.insert(11));
        assert(fast_table.contains(11));
        assert(fast_table.insert(12));
        assert(safe_table.contains(12));
        assert(safe_table.range(10, 12) == (std::vector<int>{11, 12}));
        assert(fast_table.range(12, 12) == (std::vector<int>{12}));
    }

    std::cout << "KeyTable test passed.\n";
    return 0;
}
