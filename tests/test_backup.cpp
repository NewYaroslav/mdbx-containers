#include <mdbx_containers.hpp>
#include <cstdio>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

namespace {

void cleanup(const std::string& p) {
    std::remove(p.c_str());
}

void test_backup_compact() {
    using namespace mdbxc;

    const std::string src = "test_backup_compact_src.mdbx";
    const std::string dst = "test_backup_compact_dst.mdbx";
    cleanup(src);
    cleanup(dst);

    {
        Config cfg;
        cfg.pathname = src;
        cfg.max_dbs = 4;
        cfg.no_subdir = true;
        auto conn = Connection::create(cfg);
        KeyValueTable<int, std::string> kv(conn, "t");
        for (int i = 0; i < 256; ++i) {
            kv.insert_or_assign(i, "v_" + std::to_string(i));
        }
        conn->sync_to_disk();

        BackupOptions opt;
        opt.mode = BackupMode::Compact;
        conn->backup_to(dst, opt);
    }

    {
        Config cfg;
        cfg.pathname = dst;
        cfg.read_only = true;
        cfg.no_subdir = true;
        auto conn = Connection::create(cfg);
        KeyValueTable<int, std::string> kv(conn, "t");
        std::map<int, std::string> data = kv.retrieve_all();
        if (data.size() != 256u) {
            throw std::runtime_error("expected 256 records");
        }
        if (data.at(0) != "v_0" || data.at(255) != "v_255") {
            throw std::runtime_error("record mismatch");
        }
    }

    cleanup(src);
    cleanup(dst);
}

void test_backup_normal_overwrite() {
    using namespace mdbxc;

    const std::string src = "test_backup_normal_src.mdbx";
    const std::string dst = "test_backup_normal_dst.mdbx";
    cleanup(src);
    cleanup(dst);

    {
        Config cfg;
        cfg.pathname = src;
        cfg.max_dbs = 4;
        cfg.no_subdir = true;
        auto conn = Connection::create(cfg);
        KeyValueTable<int, int> kv(conn, "t");
        for (int i = 0; i < 16; ++i) {
            kv.insert_or_assign(i, i * 7);
        }

        BackupOptions opt;
        opt.mode = BackupMode::Normal;
        conn->backup_to(dst, opt);

        std::remove(dst.c_str());

        BackupOptions opt2;
        opt2.mode = BackupMode::Normal;
        conn->backup_to(dst, opt2);
    }

    {
        Config cfg;
        cfg.pathname = dst;
        cfg.read_only = true;
        cfg.no_subdir = true;
        auto conn = Connection::create(cfg);
        KeyValueTable<int, int> kv(conn, "t");
        std::map<int, int> data = kv.retrieve_all();
        if (data.size() != 16u) {
            throw std::runtime_error("expected 16 records after rewrite");
        }
        if (data.at(8) != 56) {
            throw std::runtime_error("value mismatch at key 8");
        }
    }

    cleanup(src);
    cleanup(dst);
}

void test_sync_to_disk() {
    using namespace mdbxc;

    const std::string p = "test_sync_to_disk.mdbx";
    cleanup(p);

    {
        Config cfg;
        cfg.pathname = p;
        cfg.max_dbs = 2;
        cfg.no_subdir = true;
        auto conn = Connection::create(cfg);
        KeyValueTable<int, int> kv(conn, "t");
        kv.insert_or_assign(1, 100);
        conn->sync_to_disk(true, false);
        conn->sync_to_disk(false, true);
    }

    {
        Config cfg;
        cfg.pathname = p;
        cfg.read_only = true;
        cfg.no_subdir = true;
        auto conn = Connection::create(cfg);
        KeyValueTable<int, int> kv(conn, "t");
        if (kv.at(1) != 100) {
            throw std::runtime_error("sync_to_disk did not persist data");
        }
    }

    cleanup(p);
}

} // namespace

int main() {
    test_backup_compact();
    test_backup_normal_overwrite();
    test_sync_to_disk();
    return 0;
}