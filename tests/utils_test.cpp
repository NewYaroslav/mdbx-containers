#include "test_assert.hpp"
#include <iostream>
#include <mdbx_containers/common.hpp>

namespace {

MDBX_val empty_value() {
    MDBX_val val;
    val.iov_base = nullptr;
    val.iov_len = 0;
    return val;
}

template<typename T>
void assert_deserializes_empty() {
    T out = mdbxc::deserialize_value<T>(empty_value());
    MDBXC_TEST_ASSERT(out.empty());
}

template<typename T>
void assert_rejects_oversized_string_length() {
    const std::uint32_t len = static_cast<std::uint32_t>(~std::uint32_t(0));
    std::uint8_t raw[sizeof(len)];
    std::memcpy(raw, &len, sizeof(len));

    MDBX_val val;
    val.iov_base = raw;
    val.iov_len = sizeof(raw);

    bool thrown = false;
    try {
        (void)mdbxc::deserialize_value<T>(val);
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    MDBXC_TEST_ASSERT(thrown);
}

template<typename T>
T deserialize_unaligned_uint32_values() {
    std::vector<std::uint8_t> raw(1 + 3 * sizeof(std::uint32_t));
    std::uint32_t expected[] = {1u, 2u, 3u};
    std::memcpy(raw.data() + 1, expected, sizeof(expected));

    MDBX_val val;
    val.iov_base = raw.data() + 1;
    val.iov_len = sizeof(expected);

    return mdbxc::deserialize_value<T>(val);
}

template<typename T>
void assert_ordered_uint32_values(const T& out) {
    MDBXC_TEST_ASSERT(out.size() == 3);
    typename T::const_iterator it = out.begin();
    MDBXC_TEST_ASSERT(*it == 1u);
    ++it;
    MDBXC_TEST_ASSERT(*it == 2u);
    ++it;
    MDBXC_TEST_ASSERT(*it == 3u);
}

void assert_unordered_uint32_values(const std::unordered_set<std::uint32_t>& out) {
    MDBXC_TEST_ASSERT(out.size() == 3);
    MDBXC_TEST_ASSERT(out.find(1u) != out.end());
    MDBXC_TEST_ASSERT(out.find(2u) != out.end());
    MDBXC_TEST_ASSERT(out.find(3u) != out.end());
}

} // namespace

int main() {
    assert_deserializes_empty<std::string>();
    assert_deserializes_empty<std::vector<std::uint8_t>>();
    assert_deserializes_empty<std::deque<std::uint8_t>>();
    assert_deserializes_empty<std::list<std::uint8_t>>();
    assert_deserializes_empty<std::vector<std::uint32_t>>();
    assert_deserializes_empty<std::deque<std::uint32_t>>();
    assert_deserializes_empty<std::list<std::uint32_t>>();
    assert_deserializes_empty<std::set<std::uint32_t>>();
    assert_deserializes_empty<std::unordered_set<std::uint32_t>>();
    assert_deserializes_empty<std::vector<std::string>>();
    assert_deserializes_empty<std::deque<std::string>>();
    assert_deserializes_empty<std::list<std::string>>();
    assert_deserializes_empty<std::set<std::string>>();
    assert_deserializes_empty<std::unordered_set<std::string>>();

    assert_ordered_uint32_values(deserialize_unaligned_uint32_values<std::vector<std::uint32_t>>());
    assert_ordered_uint32_values(deserialize_unaligned_uint32_values<std::deque<std::uint32_t>>());
    assert_ordered_uint32_values(deserialize_unaligned_uint32_values<std::list<std::uint32_t>>());
    assert_ordered_uint32_values(deserialize_unaligned_uint32_values<std::set<std::uint32_t>>());
    assert_unordered_uint32_values(deserialize_unaligned_uint32_values<std::unordered_set<std::uint32_t>>());

    assert_rejects_oversized_string_length<std::vector<std::string>>();
    assert_rejects_oversized_string_length<std::deque<std::string>>();
    assert_rejects_oversized_string_length<std::list<std::string>>();
    assert_rejects_oversized_string_length<std::set<std::string>>();
    assert_rejects_oversized_string_length<std::unordered_set<std::string>>();

    return 0;
}
