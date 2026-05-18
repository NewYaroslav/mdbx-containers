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

/// \brief Custom value with a stable AnyValueTable tag specialization.
struct StableTagged {
    int value;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        return bytes;
    }

    static StableTagged from_bytes(const void* data, size_t size) {
        if (size != sizeof(int)) {
            throw std::runtime_error("Invalid data size for StableTagged");
        }
        StableTagged out{};
        std::memcpy(&out.value, data, sizeof(out.value));
        return out;
    }
};

/// \brief Compatible value using the same stable AnyValueTable tag.
struct StableTaggedAlias {
    int value;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        return bytes;
    }

    static StableTaggedAlias from_bytes(const void* data, size_t size) {
        if (size != sizeof(int)) {
            throw std::runtime_error("Invalid data size for StableTaggedAlias");
        }
        StableTaggedAlias out{};
        std::memcpy(&out.value, data, sizeof(out.value));
        return out;
    }
};

namespace mdbxc {
    template<>
    struct AnyValueTypeTag<StableTagged> {
        static const char* value() noexcept {
            return "tests.StableTagged.v1";
        }
    };

    template<>
    struct AnyValueTypeTag<StableTaggedAlias> {
        static const char* value() noexcept {
            return "tests.StableTagged.v1";
        }
    };
} // namespace mdbxc

int main() {
    mdbxc::Config cfg;
    cfg.pathname = "data/any_value_table.mdbx";
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
#if __cplusplus >= 201703L
    assert(table.find<std::string>("greeting").value() == "hello");
#else
    {
        auto res = table.find_compat<std::string>("greeting");
        assert(res.first && res.second == "hello");
    }
#endif
    assert(table.get<MyStruct>("object") == expected);
    assert(table.contains("answer"));
    assert(!table.contains("missing"));

    auto ks = table.keys();
    assert(ks.size() == 3);

    mdbxc::AnyValueTable<int> safe_int_table(conn, "test_any_int_options");
    mdbxc::AnyValueTable<int, mdbxc::FastIntegerKeyOptions> fast_int_table(conn, "test_any_int_options");
    safe_int_table.set<std::string>(11, "safe");
    fast_int_table.set<int>(12, 99);
    assert(fast_int_table.get<std::string>(11) == "safe");
    assert(safe_int_table.get<int>(12) == 99);

    mdbxc::AnyValueTable<std::string> tagged(conn, "test_any_tagged");
    tagged.erase("answer");
    tagged.erase("stable");
    tagged.set_type_tag_check(true);
    tagged.set<int>("answer", 42);
    assert(tagged.get<int>("answer") == 42);
    assert(tagged.contains("answer"));

    tagged.update<int>("answer", [](int& value) {
        value += 1;
    });
    assert(tagged.get<int>("answer") == 43);

    bool bad_cast_thrown = false;
    try {
        (void)tagged.get<std::string>("answer");
    } catch (const std::bad_cast&) {
        bad_cast_thrown = true;
    }
    assert(bad_cast_thrown);

#if __cplusplus >= 201703L
    assert(!tagged.find<std::string>("answer").has_value());
#else
    {
        auto mismatch = tagged.find_compat<std::string>("answer");
        assert(!mismatch.first);
    }
#endif
    assert(tagged.get_or<std::string>("answer", std::string("fallback")) == "fallback");

    StableTagged stable{};
    stable.value = 77;
    tagged.set<StableTagged>("stable", stable);
    StableTaggedAlias alias = tagged.get<StableTaggedAlias>("stable");
    assert(alias.value == stable.value);
    assert(std::string(mdbxc::AnyValueTypeTag<StableTagged>::value()) == "tests.StableTagged.v1");

    mdbxc::AnyValueTable<std::string> legacy(conn, "test_any_legacy_raw");
    legacy.erase("legacy");
    legacy.set<int>("legacy", 7);
    assert(legacy.get<int>("legacy") == 7);
    legacy.set_type_tag_check(true);
    bool legacy_bad_cast = false;
    try {
        (void)legacy.get<int>("legacy");
    } catch (const std::bad_cast&) {
        legacy_bad_cast = true;
    }
    assert(legacy_bad_cast);

#if __cplusplus >= 201703L
    assert(!legacy.find<int>("legacy").has_value());
#else
    {
        auto legacy_lookup = legacy.find_compat<int>("legacy");
        assert(!legacy_lookup.first);
    }
#endif

    std::cout << "AnyValueTable test passed.\n";
    return 0;
}
