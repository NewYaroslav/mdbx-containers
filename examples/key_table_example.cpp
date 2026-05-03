/**
 * \ingroup mdbxc_examples
 * Demonstrates set-like key-only tables with several key types.
 */

#include <mdbx_containers/KeyTable.hpp>

#include <bitset>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

struct RegionKey {
    uint16_t country;
    uint16_t segment;

    RegionKey() : country(0), segment(0) {}
    RegionKey(uint16_t c, uint16_t s) : country(c), segment(s) {}

    bool operator<(const RegionKey& other) const {
        if (country != other.country) {
            return country < other.country;
        }
        return segment < other.segment;
    }
};

std::ostream& operator<<(std::ostream& os, const RegionKey& key) {
    os << "country=" << key.country << ", segment=" << key.segment;
    return os;
}

template <class T>
void print_sequence(const std::string& title, const std::vector<T>& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (typename std::vector<T>::const_iterator it = values.begin(); it != values.end(); ++it) {
        std::cout << "  " << *it << '\n';
    }
}

template <class T>
void print_set(const std::string& title, const std::set<T>& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (typename std::set<T>::const_iterator it = values.begin(); it != values.end(); ++it) {
        std::cout << "  " << *it << '\n';
    }
}

int main() {
    mdbxc::Config config;
    config.pathname = "key_table_example_db";
    config.max_dbs = 8;

    auto conn = mdbxc::Connection::create(config);

    mdbxc::KeyTable<int> ids(conn, "ids");
    ids.clear();
    std::cout << "ids initially empty: " << ids.empty() << '\n';
    std::cout << "insert 10: " << ids.insert(10) << '\n';
    std::cout << "insert 10 again: " << ids.insert(10) << '\n';
    ids.insert(20);
    ids.insert(30);
    std::cout << "contains 20: " << ids.contains(20) << '\n';
    std::cout << "id count: " << ids.count() << '\n';

    std::vector<int> id_vector;
    ids.load(id_vector);
    print_sequence("loaded ids", id_vector);

    std::set<int> replacement_ids;
    replacement_ids.insert(100);
    replacement_ids.insert(200);
    ids.reconcile(replacement_ids);
    print_set("ids after reconcile", ids.retrieve_all());

    std::vector<int> appended_ids;
    appended_ids.push_back(300);
    appended_ids.push_back(400);
    ids.append(appended_ids);
    print_sequence("ids after append via operator()", ids.operator()<std::vector>());

    std::set<int> assigned_ids;
    assigned_ids.insert(1);
    assigned_ids.insert(2);
    ids = assigned_ids;
    std::cout << "ids after operator=: " << ids.count() << '\n';
    std::cout << "erase 1: " << ids.erase(1) << '\n';

    mdbxc::KeyTable<std::string> tags(conn, "tags");
    tags.clear();
    tags.insert("active");
    tags.insert("archived");
    tags.insert("billing");
    print_set("string tags", tags.retrieve_all());

    mdbxc::KeyTable<double> price_levels(conn, "price_levels");
    price_levels.clear();
    price_levels.insert(-1.5);
    price_levels.insert(0.0);
    price_levels.insert(42.25);
    std::cout << "double key contains 42.25: " << price_levels.contains(42.25) << '\n';
    std::cout << "double key count: " << price_levels.count() << '\n';

    mdbxc::KeyTable<RegionKey> regions(conn, "regions");
    regions.clear();
    regions.insert(RegionKey(1, 10));
    regions.insert(RegionKey(1, 20));
    regions.insert(RegionKey(2, 10));
    print_set("custom POD keys", regions.retrieve_all());

    typedef std::bitset<16> Flags;
    mdbxc::KeyTable<Flags> flag_table(conn, "flag_keys");
    flag_table.clear();
    Flags enabled(std::string("1010101010101010"));
    flag_table.insert(enabled);
    std::cout << "bitset key contains pattern: " << flag_table.contains(enabled) << '\n';

    mdbxc::KeyTable<std::string> manual(conn, "manual_key_txn");
    manual.clear();
    auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
    manual.insert("inside-transaction", txn);
    manual.insert("also-inside", txn);
    txn.commit();
    print_set("manual transaction keys", manual.retrieve_all());

    ids.clear();
    std::cout << "ids after clear: " << ids.count() << '\n';

    return 0;
}
