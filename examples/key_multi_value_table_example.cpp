/**
 * \ingroup mdbxc_examples
 * Demonstrates multimap-like tables with multiple values per key.
 */

#include <mdbx_containers/KeyMultiValueTable.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct AuditRecord {
    int code;
    double score;

    AuditRecord() : code(0), score(0.0) {}
    AuditRecord(int c, double s) : code(c), score(s) {}

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(code) + sizeof(score));
        std::memcpy(bytes.data(), &code, sizeof(code));
        std::memcpy(bytes.data() + sizeof(code), &score, sizeof(score));
        return bytes;
    }

    static AuditRecord from_bytes(const void* data, size_t size) {
        if (size != sizeof(int) + sizeof(double)) {
            throw std::runtime_error("Invalid data size for AuditRecord");
        }
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        AuditRecord out;
        std::memcpy(&out.code, ptr, sizeof(out.code));
        std::memcpy(&out.score, ptr + sizeof(out.code), sizeof(out.score));
        return out;
    }
};

std::ostream& operator<<(std::ostream& os, const AuditRecord& record) {
    os << "{code=" << record.code << ", score=" << record.score << "}";
    return os;
}

template <class T>
void print_values(const std::string& title, const std::vector<T>& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (typename std::vector<T>::const_iterator it = values.begin(); it != values.end(); ++it) {
        std::cout << "  " << *it << '\n';
    }
}

template <class K, class V>
void print_pairs(const std::string& title, const std::vector<std::pair<K, V> >& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (typename std::vector<std::pair<K, V> >::const_iterator it = values.begin(); it != values.end(); ++it) {
        std::cout << "  " << it->first << " -> " << it->second << '\n';
    }
}

template <class K, class V>
void print_multimap(const std::string& title, const std::multimap<K, V>& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (typename std::multimap<K, V>::const_iterator it = values.begin(); it != values.end(); ++it) {
        std::cout << "  " << it->first << " -> " << it->second << '\n';
    }
}

int main() {
    mdbxc::Config config;
    config.pathname = "key_multi_value_table_example_db";
    config.max_dbs = 8;

    auto conn = mdbxc::Connection::create(config);

    mdbxc::KeyMultiValueTable<int, std::string> events(conn, "events");
    events.clear();
    std::cout << "events initially empty: " << events.empty() << '\n';
    events.insert(7, "created");
    events.insert(7, "created");
    events.insert(7, "sent");
    events.insert(8, "queued");

    std::cout << "total event pairs: " << events.count() << '\n';
    std::cout << "values for key 7: " << events.count(7) << '\n';
    std::cout << "exact repeats of (7, created): " << events.count(7, std::string("created")) << '\n';
    std::cout << "contains key 8: " << events.contains(8) << '\n';
    std::cout << "contains (7, sent): " << events.contains(7, std::string("sent")) << '\n';
    print_values("find(7)", events.find(7));

    std::multimap<int, std::string> event_snapshot = events.retrieve_all();
    print_multimap("retrieve_all as multimap", event_snapshot);

    std::vector<std::pair<int, std::string> > event_vector;
    events.load(event_vector);
    print_pairs("load as vector", event_vector);

    std::cout << "erase exact value (7, created): " << events.erase(7, std::string("created")) << '\n';
    print_values("find(7) after exact erase", events.find(7));
    std::cout << "erase whole key 7: " << events.erase(7) << '\n';
    std::cout << "total event pairs after key erase: " << events.count() << '\n';

    std::vector<std::pair<int, std::string> > replacement_events;
    replacement_events.push_back(std::make_pair(1, std::string("opened")));
    replacement_events.push_back(std::make_pair(1, std::string("opened")));
    replacement_events.push_back(std::make_pair(1, std::string("closed")));
    events.reconcile(replacement_events);
    print_values("after reconcile, find(1)", events.find(1));

    std::multimap<int, std::string> assigned_events;
    assigned_events.insert(std::make_pair(2, std::string("assigned")));
    assigned_events.insert(std::make_pair(2, std::string("assigned")));
    events = assigned_events;
    print_multimap("after operator=", events.operator()<std::multimap>());

    std::vector<std::pair<int, std::string> > more_events;
    more_events.push_back(std::make_pair(2, std::string("reviewed")));
    more_events.push_back(std::make_pair(3, std::string("created")));
    events.append(more_events);
    print_pairs("after append", events.retrieve_all_vector());

    mdbxc::KeyMultiValueTable<std::string, int> scores(conn, "scores");
    scores.clear();
    scores.insert("alice", 10);
    scores.insert("alice", 20);
    scores.insert("bob", 15);
    print_values("scores for alice", scores.find("alice"));

    mdbxc::KeyMultiValueTable<std::string, AuditRecord> audits(conn, "audits");
    audits.clear();
    audits.insert("order-1", AuditRecord(200, 98.5));
    audits.insert("order-1", AuditRecord(201, 99.0));
    print_values("custom values for order-1", audits.find("order-1"));

    mdbxc::KeyMultiValueTable<int, std::string> manual(conn, "manual_multi_txn");
    manual.clear();
    auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
    manual.insert(42, "inside-transaction", txn);
    manual.insert(42, "inside-transaction", txn);
    txn.commit();
    print_values("manual transaction values", manual.find(42));

    events.clear();
    std::cout << "events after clear: " << events.count() << '\n';

    return 0;
}
