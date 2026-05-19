/**
 * \ingroup mdbxc_examples
 * Demonstrates a persistent appendable sequence table with stable uint64_t ids.
 */

#include <mdbx_containers/SequenceTable.hpp>

#include <iostream>
#include <list>
#include <string>
#include <vector>

int main() {
    mdbxc::Config config;
    config.pathname = "sequence_table_example_db";
    config.max_dbs = 4;

    auto conn = mdbxc::Connection::create(config);

    mdbxc::SequenceTable<std::string> events(conn, "events");
    events.clear();

    uint64_t id0 = events.append("boot");
    uint64_t id1 = events.append("init");
    uint64_t id2 = events.append("ready");

    std::cout << "appended ids: " << id0 << ", " << id1 << ", " << id2 << '\n';
    std::cout << "count: " << events.count() << '\n';

    std::cout << "at(1): " << events.at(1) << '\n';

    events.erase(1);
    std::cout << "after erase(1), count: " << events.count() << '\n';

    uint64_t id3 = events.append("shutdown");
    std::cout << "append after hole returns id: " << id3 << '\n';

    std::vector<std::string> values = events.retrieve_all();
    std::cout << "retrieve_all values (holes skipped):\n";
    for (std::vector<std::string>::const_iterator it = values.begin();
         it != values.end(); ++it) {
        std::cout << "  " << *it << '\n';
    }

    std::vector<std::pair<uint64_t, std::string>> entries = events.retrieve_entries();
    std::cout << "retrieve_entries (ids visible):\n";
    for (std::vector<std::pair<uint64_t, std::string>>::const_iterator it = entries.begin();
         it != entries.end(); ++it) {
        std::cout << "  " << it->first << " -> " << it->second << '\n';
    }

    std::vector<std::pair<uint64_t, std::string>> r = events.range(0, 2);
    std::cout << "range(0, 2) results:\n";
    for (std::vector<std::pair<uint64_t, std::string>>::const_iterator it = r.begin();
         it != r.end(); ++it) {
        std::cout << "  " << it->first << " -> " << it->second << '\n';
    }

    events.set(10, "custom_id");
    std::cout << "after set(10), count: " << events.count() << '\n';

    std::list<std::string> batch;
    batch.push_back("batch_a");
    batch.push_back("batch_b");
    std::vector<uint64_t> batch_ids = events.append_many(batch);
    std::cout << "append_many list returned ids: " << batch_ids[0] << ", " << batch_ids[1] << '\n';

    events.clear();
    std::cout << "after clear, empty: " << events.empty() << '\n';

    return 0;
}
