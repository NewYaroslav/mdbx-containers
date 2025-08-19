/**
 * \ingroup mdbxc_examples
 * Demonstrates manual transaction management.
 */

#include <mdbx_containers/KeyValueTable.hpp>
#include <iostream>
#include <limits>

int main() {
    mdbxc::Config config;
    config.pathname = "manual_txn_db";

    auto conn = mdbxc::Connection::create(config);
    mdbxc::KeyValueTable<int, std::string> table(conn, "demo");
    table.clear();

    // Start writable transaction manually
    conn->begin(mdbxc::TransactionMode::WRITABLE);

    // Insert key-value pairs
    table.insert_or_assign(10, "ten");
    table.insert_or_assign(20, "twenty");

    // Use operator[] to assign a value (modifies in-place)
    table[30] = "thirty";

    // Use operator[] to read the value
    std::cout << "Key 20 (operator[]): " << static_cast<std::string>(table[20])  << std::endl;

    // Use at() for bounds-checked access
    try {
        std::cout << "Key 30 (at): " << table.at(30) << std::endl;
    } catch (const std::out_of_range&) {
        std::cout << "Key 30 not found" << std::endl;
    }

#   if __cplusplus >= 201703L
    auto result = table.find(10);
    std::cout << "Key 10 (find): " << result.value_or("not found") << std::endl;
#   else
    auto result = table.find_compat(10);
    if (result.first)
        std::cout << "Key 10 (find): " << result.second << std::endl;
    else
        std::cout << "Key 10 (find): not found" << std::endl;
#   endif

    // Commit transaction
    conn->commit();

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
    return 0;
}
