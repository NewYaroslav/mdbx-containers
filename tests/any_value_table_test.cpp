#include <cassert>
#include <mdbx_containers/AnyValueTable.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <cstring>

/// \brief Struct to verify custom type support.
struct MyStruct {
    int a;
    double b;

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

    bool operator==(const MyStruct& other) const {
        return a == other.a && b == other.b;
    }
};

int main() {
    mdbxc::Config cfg;
    cfg.pathname = "any_value_table_test.mdbx";
    cfg.max_dbs = 8;
    cfg.no_subdir = true;
    cfg.relative_to_exe = true;
    auto conn = mdbxc::Connection::create(cfg);

    mdbxc::AnyValueTable<std::string> table(conn, "test_any");
    table.set<int>("answer", 42);
    table.set<std::string>("greeting", "hello");
    MyStruct expected{7, 3.5};
    table.set<MyStruct>("object", expected);

    assert(table.get<int>("answer") == 42);
    assert(table.find<std::string>("greeting").value() == "hello");
    assert(table.get<MyStruct>("object") == expected);

    auto ks = table.keys();
    assert(ks.size() == 3);

    std::cout << "AnyValueTable test passed.\n";
    return 0;
}
