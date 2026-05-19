/**
 * \ingroup mdbxc_examples
 * Shows one RAII transaction shared by several table classes.
 */

#include <mdbx_containers/KeyValueTable.hpp>
#include <mdbx_containers/ValueTable.hpp>

#include <iostream>
#include <string>

int main() {
    mdbxc::Config config;
    config.pathname = "single_transaction_demo.mdbx";
    config.max_dbs = 4;

    auto conn = mdbxc::Connection::create(config);
    mdbxc::KeyValueTable<int, std::string> users(conn, "users");
    mdbxc::ValueTable<int> schema_version(conn, "schema_version");

    auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
    users.clear(txn);
    schema_version.clear(txn);
    users.insert_or_assign(1, "Ada", txn);
    users.insert_or_assign(2, "Bjarne", txn);
    schema_version.set(1, txn);
    txn.commit();

    auto read_txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
    std::cout << "User 1: " << users.at(1, read_txn) << '\n';
    std::cout << "Schema version: " << schema_version.get(read_txn) << '\n';
    read_txn.commit();

    return 0;
}
