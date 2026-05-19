#include <cassert>
#include <cstring>
#include <iostream>
#include <list>
#include <stdexcept>
#include <string>
#include <vector>

#include <mdbx_containers/SequenceTable.hpp>

/// \brief Custom serializable struct used by SequenceTable tests.
struct Profile {
    int id;
    std::string name;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(int) + sizeof(uint32_t) + name.size());
        std::memcpy(bytes.data(), &id, sizeof(int));
        uint32_t name_len = static_cast<uint32_t>(name.size());
        std::memcpy(bytes.data() + sizeof(int), &name_len, sizeof(uint32_t));
        std::memcpy(bytes.data() + sizeof(int) + sizeof(uint32_t),
                    name.data(), name.size());
        return bytes;
    }

    static Profile from_bytes(const void* data, size_t size) {
        Profile out;
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        if (size < sizeof(int) + sizeof(uint32_t)) {
            throw std::runtime_error("Invalid data size for Profile");
        }
        std::memcpy(&out.id, ptr, sizeof(int));
        uint32_t name_len;
        std::memcpy(&name_len, ptr + sizeof(int), sizeof(uint32_t));
        if (size != sizeof(int) + sizeof(uint32_t) + name_len) {
            throw std::runtime_error("Invalid data size for Profile");
        }
        out.name.assign(
            reinterpret_cast<const char*>(ptr + sizeof(int) + sizeof(uint32_t)),
            name_len);
        return out;
    }

    bool operator==(const Profile& other) const {
        return id == other.id && name == other.name;
    }
};

int main() {
    mdbxc::Config cfg;
    cfg.pathname = "data/sequence_table_test.mdbx";
    cfg.max_dbs = 10;
    cfg.no_subdir = true;
    cfg.relative_to_exe = true;

    auto conn = mdbxc::Connection::create(cfg);

    // --- 1. basic_append_and_read ---
    {
        mdbxc::SequenceTable<std::string> table(conn, "seq_basic");
        table.clear();

        assert(table.empty());
        assert(table.count() == 0);

        uint64_t id0 = table.append("alpha");
        uint64_t id1 = table.append("beta");
        assert(id0 == 0);
        assert(id1 == 1);
        assert(table.count() == 2);
        assert(!table.empty());

        assert(table.at(0) == "alpha");
        assert(table.at(1) == "beta");

        assert(table.contains(0));
        assert(table.contains(1));
        assert(!table.contains(99));

        std::string out;
        assert(table.try_get(0, out) && out == "alpha");
        assert(!table.try_get(99, out));

        bool threw = false;
        try {
            (void)table.at(99);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        assert(threw);

        std::pair<bool, std::string> fc = table.find_compat(0);
        assert(fc.first && fc.second == "alpha");
        fc = table.find_compat(99);
        assert(!fc.first);

#if __cplusplus >= 201703L
        auto found = table.find(0);
        assert(found.has_value() && *found == "alpha");
        found = table.find(99);
        assert(!found.has_value());
#endif
    }

    // --- 2. erase_and_holes ---
    {
        mdbxc::SequenceTable<std::string> table(conn, "seq_holes");
        table.clear();

        uint64_t id0 = table.append("zero");
        uint64_t id1 = table.append("one");
        uint64_t id2 = table.append("two");
        assert(id0 == 0);
        assert(id1 == 1);
        assert(id2 == 2);

        assert(table.erase(1));
        assert(!table.erase(1));
        assert(table.count() == 2);

        std::vector<std::string> vals = table.retrieve_all();
        assert(vals.size() == 2);
        assert(vals[0] == "zero");
        assert(vals[1] == "two");

        std::vector<std::pair<uint64_t, std::string>> entries = table.retrieve_entries();
        assert(entries.size() == 2);
        assert(entries[0].first == 0 && entries[0].second == "zero");
        assert(entries[1].first == 2 && entries[1].second == "two");

        uint64_t id3 = table.append("three");
        assert(id3 == 3);
    }

    // --- 3. set_insert_or_assign ---
    {
        mdbxc::SequenceTable<std::string> table(conn, "seq_set");
        table.clear();

        table.set(10, "ten");
        assert(table.count() == 1);

        std::pair<bool, uint64_t> first = table.first_index_compat();
        std::pair<bool, uint64_t> last = table.last_index_compat();
        assert(first.first && first.second == 10);
        assert(last.first && last.second == 10);

        uint64_t id_next = table.append("eleven");
        assert(id_next == 11);

        table.insert_or_assign(10, "TEN");
        assert(table.at(10) == "TEN");

#if __cplusplus >= 201703L
        auto fi = table.first_index();
        assert(fi.has_value() && *fi == 10);
        auto li = table.last_index();
        assert(li.has_value() && *li == 11);
#endif
    }

    // --- 4. range ---
    {
        mdbxc::SequenceTable<std::string> table(conn, "seq_range");
        table.clear();

        table.set(2, "v2");
        table.set(4, "v4");
        table.set(5, "v5");
        table.set(10, "v10");

        std::vector<std::pair<uint64_t, std::string>> r;

        r = table.range(3, 6);
        assert(r.size() == 2);
        assert(r[0].first == 4 && r[0].second == "v4");
        assert(r[1].first == 5 && r[1].second == "v5");

        r = table.range(0, 2);
        assert(r.size() == 1);
        assert(r[0].first == 2 && r[0].second == "v2");

        r = table.range(11, 20);
        assert(r.empty());

        r = table.range(7, 3);
        assert(r.empty());
    }

    // --- 5. append_many ---
    {
        mdbxc::SequenceTable<int> table(conn, "seq_many");
        table.clear();

        std::vector<int> src;
        src.push_back(1);
        src.push_back(2);
        src.push_back(3);

        std::vector<uint64_t> ids = table.append_many(src);
        assert(ids.size() == 3);
        assert(ids[0] == 0);
        assert(ids[1] == 1);
        assert(ids[2] == 2);

        std::vector<int> all = table.retrieve_all();
        assert(all.size() == 3);
        assert(all[0] == 1);
        assert(all[1] == 2);
        assert(all[2] == 3);

        // Test with std::list<int>
        mdbxc::SequenceTable<int> table2(conn, "seq_many_list");
        table2.clear();

        std::list<int> lst;
        lst.push_back(10);
        lst.push_back(20);
        lst.push_back(30);

        std::vector<uint64_t> ids2 = table2.append_many(lst);
        assert(ids2.size() == 3);
        assert(ids2[0] == 0);
        assert(ids2[1] == 1);
        assert(ids2[2] == 2);

        std::vector<int> all2 = table2.retrieve_all();
        assert(all2.size() == 3);
        assert(all2[0] == 10);
        assert(all2[1] == 20);
        assert(all2[2] == 30);
    }

    // --- 6. transaction overloads ---
    {
        mdbxc::SequenceTable<std::string> table(conn, "seq_txn");
        table.clear();

        auto txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        uint64_t id = table.append("txn_val", txn);
        assert(id == 0);
        txn.commit();

        auto read_txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        assert(table.count(read_txn) == 1);
        assert(table.at(0, read_txn) == "txn_val");
        std::vector<std::pair<uint64_t, std::string>> r =
            table.range(0, 10, read_txn);
        assert(r.size() == 1);
        assert(r[0].first == 0 && r[0].second == "txn_val");
        read_txn.commit();
    }

    // --- 7. clear ---
    {
        mdbxc::SequenceTable<std::string> table(conn, "seq_clear");
        table.clear();

        table.append("a");
        table.append("b");
        table.append("c");
        assert(table.count() == 3);

        table.clear();
        assert(table.empty());
        assert(table.count() == 0);

        std::pair<bool, uint64_t> first = table.first_index_compat();
        std::pair<bool, uint64_t> last = table.last_index_compat();
        assert(!first.first);
        assert(!last.first);

#if __cplusplus >= 201703L
        auto fi = table.first_index();
        auto li = table.last_index();
        assert(!fi.has_value());
        assert(!li.has_value());
#endif
    }

    // --- 8. custom serializable struct ---
    {
        mdbxc::SequenceTable<Profile> table(conn, "seq_profile");
        table.clear();

        Profile p;
        p.id = 7;
        p.name = "alice";
        uint64_t id = table.append(p);
        assert(id == 0);

        Profile loaded = table.at(0);
        assert(loaded == p);

        assert(table.contains(0));
        assert(!table.contains(1));

        Profile p2;
        p2.id = 42;
        p2.name = "bob";
        table.insert_or_assign(5, p2);
        Profile loaded2 = table.at(5);
        assert(loaded2 == p2);

        std::pair<bool, Profile> fc = table.find_compat(5);
        assert(fc.first && fc.second == p2);
    }

    // --- 9. Config constructor ---
    {
        mdbxc::Config standalone_cfg;
        standalone_cfg.pathname = "data/sequence_table_config_test.mdbx";
        standalone_cfg.max_dbs = 2;
        standalone_cfg.no_subdir = true;
        standalone_cfg.relative_to_exe = true;

        mdbxc::SequenceTable<std::string> table(standalone_cfg, "config_seq");
        table.clear();
        uint64_t id = table.append("config_test");
        assert(id == 0);
        assert(table.at(0) == "config_test");
    }

    std::cout << "SequenceTable test passed.\n";
    return 0;
}
