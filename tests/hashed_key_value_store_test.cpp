#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <mdbx_containers/HashedKeyValueStore.hpp>

#if __cplusplus >= 201703L
#   include <cstddef>
#endif

namespace {

struct ConstantHasher {
    std::uint64_t operator()(mdbxc::ByteView) const noexcept {
        return 42;
    }
};

template<class Store, class Key, class Value>
void assert_found(const Store& store, const Key& key, const Value& expected) {
#if __cplusplus >= 201703L
    std::optional<Value> found = store.find(key);
    assert(found.has_value());
    assert(*found == expected);
#else
    std::pair<bool, Value> found = store.find_compat(key);
    assert(found.first);
    assert(found.second == expected);
#endif
}

} // namespace

int main() {
    mdbxc::Config cfg;
    cfg.pathname = "data/hashed_key_value_store_test.mdbx";
    cfg.max_dbs = 32;
    cfg.no_subdir = true;
    cfg.relative_to_exe = true;

    auto conn = mdbxc::Connection::create(cfg);

    {
        mdbxc::HashedKeyValueStore<std::string, int> store(conn, "hashed_strings");
        store.clear();

        assert(store.empty());
        assert(store.insert("alpha", 1));
        assert(!store.insert("alpha", 11));
        store.insert_or_assign("beta", 2);
        assert_found<decltype(store), std::string, int>(store, "alpha", 1);
        assert(store.at("beta") == 2);

        int out = 0;
        assert(store.try_get("alpha", out));
        assert(out == 1);
        assert(!store.try_get("missing", out));
        assert(store.contains("alpha"));
        assert(!store.contains("missing"));

        store["gamma"] = 3;
        assert(static_cast<int>(store["gamma"]) == 3);
        assert(static_cast<int>(store["defaulted"]) == 0);
        assert(store.count() == 4);

        std::map<std::string, int> as_map;
        store.load(as_map);
        assert(as_map["alpha"] == 1);
        assert(as_map["beta"] == 2);
        assert(as_map["gamma"] == 3);

        std::vector<std::pair<std::string, int> > as_vector = store.retrieve_all<std::vector>();
        assert(as_vector.size() == 4);

        assert(store.erase("alpha"));
        assert(!store.erase("alpha"));
        assert(!store.contains("alpha"));
        assert(store.count() == 3);
    }

    {
        mdbxc::HashedKeyValueStore<std::string, int> store(conn, "hashed_bulk");
        store.clear();

        std::map<std::string, int> source;
        source["one"] = 1;
        source["two"] = 2;
        store.reconcile(source);
        assert(store.count() == 2);

        std::vector<std::pair<std::string, int> > additions;
        additions.push_back(std::make_pair(std::string("two"), 20));
        additions.push_back(std::make_pair(std::string("three"), 3));
        store.append(additions);
        assert(store.at("two") == 20);
        assert(store.at("three") == 3);

        std::vector<std::pair<std::string, int> > replacement;
        replacement.push_back(std::make_pair(std::string("two"), 200));
        replacement.push_back(std::make_pair(std::string("two"), 201));
        replacement.push_back(std::make_pair(std::string("four"), 4));
        store = replacement;
        assert(store.count() == 2);
        assert(store.at("two") == 201);
        assert(store.at("four") == 4);
        assert(!store.contains("one"));
        assert(!store.contains("three"));
    }

    {
        mdbxc::HashedKeyValueStore<std::string, std::string, ConstantHasher> store(
            conn,
            "hashed_collisions",
            ConstantHasher()
        );
        store.clear();

        assert(store.insert("alpha", "one"));
        assert(store.insert("beta", "two"));
        assert(store.insert("gamma", "three"));
        store.insert_or_assign("beta", "TWO");

        assert_found<decltype(store), std::string, std::string>(store, "alpha", "one");
        assert_found<decltype(store), std::string, std::string>(store, "beta", "TWO");
        assert_found<decltype(store), std::string, std::string>(store, "gamma", "three");
        assert(store.erase("alpha"));
        assert(!store.contains("alpha"));
        assert(store.contains("beta"));
        assert(store.count() == 2);
    }

    {
        typedef std::vector<uint8_t> Bytes;
        mdbxc::HashedKeyValueStore<Bytes, std::string> store(conn, "hashed_bytes");
        store.clear();

        Bytes first;
        first.push_back(0);
        first.push_back(1);
        first.push_back(2);

        Bytes second;
        second.push_back(0);
        second.push_back(1);
        second.push_back(3);

        assert(store.insert(first, "first"));
        store.insert_or_assign(second, "second");
        assert_found<decltype(store), Bytes, std::string>(store, first, "first");
        assert_found<decltype(store), Bytes, std::string>(store, second, "second");

        std::map<Bytes, std::string> as_map = store.retrieve_all();
        assert(as_map[first] == "first");
        assert(as_map[second] == "second");
    }

    {
        typedef std::vector<char> CharBytes;
        mdbxc::HashedKeyValueStore<CharBytes, int> store(conn, "hashed_char_bytes");
        store.clear();

        CharBytes key;
        key.push_back('a');
        key.push_back('\0');
        key.push_back('b');

        store.insert_or_assign(key, 7);
        assert_found<decltype(store), CharBytes, int>(store, key, 7);
    }

#if __cplusplus >= 201703L
    {
        typedef std::vector<std::byte> StdBytes;
        mdbxc::HashedKeyValueStore<StdBytes, std::string> store(conn, "hashed_std_bytes");
        store.clear();

        StdBytes key;
        key.push_back(std::byte{0x01});
        key.push_back(std::byte{0x02});
        key.push_back(std::byte{0xff});

        store.insert_or_assign(key, "byte-key");
        assert_found<decltype(store), StdBytes, std::string>(store, key, "byte-key");

        std::map<StdBytes, std::string> as_map = store.retrieve_all();
        assert(as_map[key] == "byte-key");
    }
#endif

    {
        mdbxc::SipHashHasher sip(UINT64_C(0x0706050403020100), UINT64_C(0x0f0e0d0c0b0a0908));
        mdbxc::HashedKeyValueStore<std::string, int, mdbxc::SipHashHasher> store(conn, "hashed_sip", sip);
        store.clear();
        store.insert_or_assign("token", 123);

        mdbxc::HashedKeyValueStore<std::string, int, mdbxc::SipHashHasher> reopened(
            conn,
            "hashed_sip",
            mdbxc::SipHashHasher(UINT64_C(0x0706050403020100), UINT64_C(0x0f0e0d0c0b0a0908))
        );
        assert(reopened.contains("token"));
        assert(reopened.at("token") == 123);

        mdbxc::HashedKeyValueStore<std::string, int, mdbxc::SipHashHasher> wrong_key(
            conn,
            "hashed_sip",
            mdbxc::SipHashHasher(1, 2)
        );
        assert(wrong_key.count() == 1);
        assert(!wrong_key.contains("token"));
    }

    {
        mdbxc::HashedKeyValueStore<std::string, std::string> store(conn, "hashed_txn");
        store.clear();

        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        assert(store.insert("one", "1", txn));
        store.insert_or_assign("two", "2", txn);
        txn.commit();

        auto read_txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        assert(store.count(read_txn) == 2);
        assert(store.at("one", read_txn) == "1");
        assert(store.contains("two", read_txn));
        read_txn.commit();
    }

    {
        mdbxc::HashedKeyValueStore<std::string, std::string> store(conn, "hashed_large_value");
        store.clear();

        std::string large(64 * 1024, 'x');
        large[0] = 'A';
        large[large.size() - 1] = 'Z';
        store.insert_or_assign("large", large);
        assert(store.at("large") == large);
    }

    std::cout << "HashedKeyValueStore test passed.\n";
    return 0;
}
