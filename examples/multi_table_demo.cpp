/**
 * \ingroup mdbxc_examples
 * Using multiple tables in a single environment.
 */

#include <mdbx_containers/KeyValueTable.hpp>
#include <mdbx_containers/KeyTable.hpp>
#include <iostream>

int main() {
    mdbxc::Config config;
    config.pathname = "multi_table_db";
    config.max_dbs = 2;

    auto conn = mdbxc::Connection::create(config);

    mdbxc::KeyValueTable<int, std::string> int_to_str_table(conn, "kv_table1");
    mdbxc::KeyValueTable<std::string, std::string> str_to_str_table(conn, "kv_table2");
    
    int_to_str_table.clear();
    str_to_str_table.clear();

    int_to_str_table.insert_or_assign(100, "hundred");
    str_to_str_table.insert_or_assign("a", "b");

#   if __cplusplus >= 201703L
    auto val1 = int_to_str_table.find(100);
    auto val2 = str_to_str_table.find("a");
    if (val1)
        std::cout << "kv_table1[100]: " << *val1 << std::endl;
    else
        std::cout << "kv_table1[100]: not found" << std::endl;

    if (val2)
        std::cout << "kv_table2[\"a\"]: " << *val2 << std::endl;
    else
        std::cout << "kv_table2[\"a\"]: not found" << std::endl;
#   else
    auto val1 = int_to_str_table.find_compat(100);
    auto val2 = str_to_str_table.find_compat("a");
    if (val1.first)
        std::cout << "kv_table1[100]: " << val1.second << std::endl;
    else
        std::cout << "kv_table1[100]: not found" << std::endl;

    if (val2.first)
        std::cout << "kv_table2[\"a\"]: " << val2.second << std::endl;
    else
        std::cout << "kv_table2[\"a\"]: not found" << std::endl;
#   endif

    return 0;
}
