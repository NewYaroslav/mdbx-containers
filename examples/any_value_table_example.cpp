/**
 * \ingroup mdbxc_examples
 * Demonstrates storing values of arbitrary types using AnyValueTable.
 */

#include <mdbx_containers/AnyValueTable.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>

/// \brief Simple struct for demonstration.
struct MyStruct {
    int a;
    float b;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(MyStruct));
        std::memcpy(bytes.data(), this, sizeof(MyStruct));
        return bytes;
    }

    static MyStruct from_bytes(const void* data, size_t size) {
        if (size != sizeof(MyStruct)) {
            throw std::runtime_error("Invalid data size for MyStruct");
        }
        MyStruct out{};
        std::memcpy(&out, data, sizeof(MyStruct));
        return out;
    }
};

/// \brief Entry point demonstrating AnyValueTable.
int main() {
    mdbxc::Config cfg;
    cfg.pathname = "any_value_table_example_db";
    cfg.max_dbs = 4;
    auto conn = mdbxc::Connection::create(cfg);

    mdbxc::AnyValueTable<std::string> table(conn, "settings");

    table.set<int>("retries", 3);
    table.set<std::string>("url", "https://example.com");
    table.set<MyStruct>("struct", MyStruct{42, 0.5f});

    auto retries = table.get_or<int>("retries", 1);
    auto url = table.find<std::string>("url").value_or("none");

    std::cout << "retries: " << retries << "\nurl: " << url << '\n';

    for (auto& key : table.keys()) {
        std::cout << "key: " << key << '\n';
    }

    return 0;
}
