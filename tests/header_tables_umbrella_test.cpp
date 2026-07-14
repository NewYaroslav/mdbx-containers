#include <mdbx_containers/tables.hpp>

#include "test_assert.hpp"

#include <cstdint>
#include <string>

int main() {
    mdbxc::AnyValueTable<std::uint64_t>* any_value_table = nullptr;
    MDBXC_TEST_ASSERT(any_value_table == nullptr);

    mdbxc::KeyTable<std::uint64_t>* key_table = nullptr;
    MDBXC_TEST_ASSERT(key_table == nullptr);

    mdbxc::KeyValueTable<std::uint64_t, std::string>* key_value_table = nullptr;
    MDBXC_TEST_ASSERT(key_value_table == nullptr);

    mdbxc::KeyMultiValueTable<std::uint64_t, std::string>* multi_value_table = nullptr;
    MDBXC_TEST_ASSERT(multi_value_table == nullptr);

    mdbxc::SequenceTable<std::uint64_t>* sequence_table = nullptr;
    MDBXC_TEST_ASSERT(sequence_table == nullptr);

    mdbxc::ValueTable<std::string>* value_table = nullptr;
    MDBXC_TEST_ASSERT(value_table == nullptr);

    mdbxc::HashedKeyValueStore<std::string, std::string>* hashed_store = nullptr;
    MDBXC_TEST_ASSERT(hashed_store == nullptr);

    const std::string key = "tables";
    const mdbxc::ByteView view = mdbxc::make_byte_view(key);
    MDBXC_TEST_ASSERT(view.data != nullptr);
    MDBXC_TEST_ASSERT(view.size == key.size());

    const mdbxc::XXH3Hasher hasher;
    const std::uint64_t digest = hasher(view);
    MDBXC_TEST_ASSERT(digest == hasher(view));

    return 0;
}
