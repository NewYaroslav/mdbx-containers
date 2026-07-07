#include <mdbx_containers.hpp>
#include <cstdio>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#if __cplusplus >= 201703L
# include <filesystem>
# include <system_error>
#endif

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

void test_sync_to_disk_readonly_throws() {
    using namespace mdbxc;

    const std::string p = "test_sync_to_disk_readonly.mdbx";
    cleanup(p);

    {
        Config cfg;
        cfg.pathname = p;
        cfg.max_dbs = 2;
        cfg.no_subdir = true;
        auto conn = Connection::create(cfg);
        KeyValueTable<int, int> kv(conn, "t");
        kv.insert_or_assign(1, 100);
    }

    {
        Config cfg;
        cfg.pathname = p;
        cfg.read_only = true;
        cfg.no_subdir = true;
        auto conn = Connection::create(cfg);
        bool caught = false;
        try {
            conn->sync_to_disk(true, false);
        } catch (const MdbxException&) {
            caught = true;
        } catch (const std::runtime_error&) {
            caught = true;
        }
        if (!caught) {
            throw std::runtime_error("sync_to_disk on read-only connection should throw");
        }
    }

    cleanup(p);
}

#if __cplusplus >= 201703L
void test_backup_directory_mode() {
    using namespace mdbxc;
    namespace fs = std::filesystem;

    const fs::path src_dir = "test_backup_dir_src";
    const fs::path dst_dir = "test_backup_dir_dst";
    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);

    {
        Config cfg;
        cfg.pathname = src_dir.string();
        cfg.max_dbs = 4;
        cfg.no_subdir = false;
        auto conn = Connection::create(cfg);
        KeyValueTable<int, std::string> kv(conn, "t");
        for (int i = 0; i < 32; ++i) {
            kv.insert_or_assign(i, "d_" + std::to_string(i));
        }
        conn->sync_to_disk();

        BackupOptions opt;
        opt.mode = BackupMode::Compact;
        conn->backup_to(dst_dir.string(), opt);
    }

    {
        Config cfg;
        cfg.pathname = dst_dir.string();
        cfg.read_only = true;
        cfg.no_subdir = false;
        auto conn = Connection::create(cfg);
        KeyValueTable<int, std::string> kv(conn, "t");
        std::map<int, std::string> data = kv.retrieve_all();
        if (data.size() != 32u) {
            throw std::runtime_error("directory backup: expected 32 records");
        }
        if (data.at(7) != "d_7") {
            throw std::runtime_error("directory backup: value mismatch at key 7");
        }
    }

    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
#endif

} // namespace

int main() {
    test_backup_compact();
    test_backup_normal_overwrite();
    test_sync_to_disk();
    test_sync_to_disk_readonly_throws();
#if __cplusplus >= 201703L
    test_backup_directory_mode();
#endif
    return 0;
}