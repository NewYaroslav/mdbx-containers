#include <mdbx_containers/KeyValueTable.hpp>
#include <iostream>
#include <string>

int main() {
    mdbxc::Config config;
    config.pathname = "full_methods_db";
    config.max_dbs = 1;

    auto conn = mdbxc::Connection::create(config);
    mdbxc::KeyValueTable<int, std::string> table(conn, "full_demo");

    // insert_or_assign
    table.insert_or_assign(1, "one");
    table.insert_or_assign(2, "two");

    // insert (no overwrite)
    bool inserted = table.insert(3, "three");
    std::cout << "Inserted key 3: " << inserted << std::endl;

    inserted = table.insert(2, "TWO");
    std::cout << "Inserted key 2 again: " << inserted << " (should be false)" << std::endl;

    // contains
    std::cout << "Contains key 1: " << table.contains(1) << std::endl;
    std::cout << "Contains key 4: " << table.contains(4) << std::endl;

    // operator[]
    table[4] = "four";
    std::cout << "table[4]: " << static_cast<std::string>(table[4]) << std::endl;

    // operator()
    std::map<int, std::string> snapshot = table();
    std::cout << "Snapshot:\n";
    for (const auto& [k, v] : snapshot)
        std::cout << k << ": " << v << std::endl;
    
    // operator=
    std::unordered_map<int, std::string> temp_data = {
        {100, "hundred"},
        {200, "two hundred"}
    };
    table = temp_data;
    
    // retrieve_all()
    std::map<int, std::string> snapshot_2 = table.retrieve_all();

#   if __cplusplus >= 201703L
    // find
    auto result = table.find(100);
    if (result)
        std::cout << "Found key 100: " << *result << std::endl;
    else
        std::cout << "Key 100 not found\n";
#   else
    auto result = table.find_compat(100);
    if (result.first)
        std::cout << "Found key 100: " << result.second << std::endl;
    else
        std::cout << "Key 100 not found\n";
#   endif

    // erase
    bool erased = table.erase(200);
    std::cout << "Erased key 200: " << erased << std::endl;

    // load
    std::cout << "All key-value pairs:\n";
    std::vector<std::pair<int, std::string>> all;
    table.load(all);
    for (size_t i = 0; i < all.size(); ++i)
        std::cout << all[i].first << ": " << all[i].second << std::endl;

    // clear
    table.clear();
    std::cout << "After clear, size = " << table.count() << std::endl;

    return 0;
}
