#include <mdbx_containers/KeyValueTable.hpp>
#include <iostream>
#include <vector>
#include <cstring>

struct MyStruct {
    int a;
    float b;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> out(sizeof(MyStruct));
        std::memcpy(out.data(), this, sizeof(MyStruct));
        return out;
    }

    static MyStruct from_bytes(const void* data, size_t size) {
        MyStruct out;
        if (size >= sizeof(MyStruct))
            std::memcpy(&out, data, sizeof(MyStruct));
        return out;
    }
};

int main() {
    mdbxc::Config config;
    config.pathname = "example_db";

    auto conn = mdbxc::Connection::create(config);
    mdbxc::KeyValueTable<int, std::string> table(conn, "demo");

    auto txn = conn->transaction();
    table.clear(txn);
    table.insert_or_assign(1, "one", txn);
    table.insert_or_assign(2, "two", txn);

#   if __cplusplus >= 201703L
    auto result = table.find(1, txn);
    std::cout << "Key 1: " << result.value_or("not found") << std::endl;
#   else
    auto result = table.find_compat(1, txn);
    if (result.first)
        std::cout << "Key 1: " << result.second << std::endl;
    else
        std::cout << "Key 1: not found" << std::endl;
#   endif

    txn.commit();
    return 0;
}
