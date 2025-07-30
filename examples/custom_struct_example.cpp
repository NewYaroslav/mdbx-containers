/**
 * \ingroup mdbxc_examples
 * Storing a custom struct with to_bytes/from_bytes.
 */

#include <mdbx_containers/KeyValueTable.hpp>
#include <iostream>
#include <vector>

struct MyData {
    int id;
    double value;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(MyData));
        std::memcpy(bytes.data(), this, sizeof(MyData));
        return bytes;
    }

    static MyData from_bytes(const void* data, size_t size) {
        if (size != sizeof(MyData))
            throw std::runtime_error("Invalid data size for MyData");
        MyData out;
        std::memcpy(&out, data, sizeof(MyData));
        return out;
    }
};

int main() {
    mdbxc::Config config;
    config.pathname = "custom_struct_db";
    config.max_dbs = 1;
    auto conn = mdbxc::Connection::create(config);

    mdbxc::KeyValueTable<int, MyData> table(conn, "my_data");
    table.clear();
    table.insert_or_assign(42, MyData{42, 3.14});
#   if __cplusplus >= 201703L
    auto result = table.find(42);
    if (result)
        std::cout << "id: " << result->id << ", value: " << result->value << std::endl;
#   else
    auto result = table.find_compat(42);
    if (result.first)
        std::cout << "id: " << result.second.id << ", value: " << result.second.value << std::endl;
#   endif
}
