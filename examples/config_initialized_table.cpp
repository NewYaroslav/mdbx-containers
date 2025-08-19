/**
 * \ingroup mdbxc_examples
 * Basic example using Config to initialize a table.
 */

#include <mdbx_containers/KeyValueTable.hpp>
#include <iostream>
#include <limits>

int main() {
    mdbxc::Config config;
    config.pathname = "one_table_db";
    config.max_dbs = 1;

    mdbxc::KeyValueTable<int, std::string> table(config, "single_table");
    table.clear();
    table.insert_or_assign(1, "example");
#   if __cplusplus >= 201703L
    auto val = table.find(1);
    std::cout << "Found: " << (val ? *val : "not found") << std::endl;
#   else
    auto val = table.find_compat(1);
    std::cout << "Found: " << (val.first ? val.second : "not found") << std::endl;
#   endif

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
}
