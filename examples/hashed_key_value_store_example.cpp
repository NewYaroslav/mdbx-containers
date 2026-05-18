/**
 * \ingroup mdbxc_examples
 * Hash-indexed key-value store with string keys.
 */

#include <mdbx_containers/HashedKeyValueStore.hpp>

#include <iostream>
#include <string>

int main() {
    mdbxc::Config config;
    config.pathname = "hashed_key_value_store_example.mdbx";
    config.max_dbs = 5; // LargeValues uses two DBIs; SmallValues uses one.

    auto conn = mdbxc::Connection::create(config);

    mdbxc::HashedKeyValueStore<std::string, std::string> local(conn, "local");
    local.clear();
    local.insert_or_assign("path:/tmp/report.txt", "seen");
    local.insert_or_assign("url:https://example.test", "queued");

    std::cout << "Local path state: " << local.at("path:/tmp/report.txt") << "\n";

    mdbxc::SipHashHasher sip(UINT64_C(0x0706050403020100), UINT64_C(0x0f0e0d0c0b0a0908));
    mdbxc::HashedKeyValueStore<std::string, std::string, mdbxc::SipHashHasher>
        untrusted(conn, "untrusted", sip);
    untrusted.clear();
    untrusted.insert_or_assign("external-token", "accepted");

    std::cout << "Token state: " << untrusted.at("external-token") << "\n";

    typedef mdbxc::HashedKeyValueStore<
        std::string,
        int,
        mdbxc::XXH3Hasher,
        mdbxc::HashedStoreLayout::SmallValues> SmallStore;
    SmallStore counters(conn, "small_counters");
    counters.clear();
    counters.insert_or_assign("hits", 7);

    std::cout << "Small counter: " << counters.at("hits") << "\n";
    return 0;
}
