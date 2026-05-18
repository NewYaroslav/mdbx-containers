/**
 * \ingroup mdbxc_examples
 * Demonstrates storing values of arbitrary types using AnyValueTable.
 */

#include <mdbx_containers/AnyValueTable.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <limits>

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

/// \brief Small value type with an application-defined stable tag.
struct TaggedCounter {
    int value;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        return bytes;
    }

    static TaggedCounter from_bytes(const void* data, size_t size) {
        if (size != sizeof(int)) {
            throw std::runtime_error("Invalid data size for TaggedCounter");
        }
        TaggedCounter out{};
        std::memcpy(&out.value, data, sizeof(out.value));
        return out;
    }
};

namespace mdbxc {
    template<>
    struct AnyValueTypeTag<TaggedCounter> {
        static const char* value() noexcept {
            return "example.TaggedCounter.v1";
        }
    };
} // namespace mdbxc

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
#if __cplusplus >= 201703L
    auto url = table.find<std::string>("url").value_or("none");
#else
    auto url_pair = table.find_compat<std::string>("url");
    std::string url = url_pair.first ? url_pair.second : "none";
#endif

    std::cout << "retries: " << retries << "\nurl: " << url << '\n';

    for (auto& key : table.keys()) {
        std::cout << "key: " << key << '\n';
    }

    mdbxc::AnyValueTable<std::string> checked(conn, "checked_settings");
    checked.erase("counter");
    checked.erase("legacy_raw");

    checked.set_type_tag_check(true);
    TaggedCounter counter{10};
    checked.set<TaggedCounter>("counter", counter);

    TaggedCounter restored = checked.get<TaggedCounter>("counter");
    std::cout << "tagged counter: " << restored.value << '\n';
    std::cout << "stable tag: " << mdbxc::AnyValueTypeTag<TaggedCounter>::value() << '\n';

    try {
        (void)checked.get<int>("counter");
    } catch (const std::bad_cast&) {
        std::cout << "wrong tagged type rejected by get<T>()\n";
    }

#if __cplusplus >= 201703L
    if (!checked.find<int>("counter")) {
        std::cout << "wrong tagged type is missing for find<T>()\n";
    }
#else
    auto wrong_type = checked.find_compat<int>("counter");
    if (!wrong_type.first) {
        std::cout << "wrong tagged type is missing for find_compat<T>()\n";
    }
#endif

    checked.set_type_tag_check(false);
    checked.set<int>("legacy_raw", 5);
    checked.set_type_tag_check(true);
    if (checked.get_or<int>("legacy_raw", -1) == -1) {
        std::cout << "raw legacy value is hidden while tag checking is enabled\n";
    }

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
    return 0;
}
