#include <mdbx_containers.hpp>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>

int main() {
    using namespace mdbxc;

    const std::string db_path = "backup_basic_source.mdbx";
    const std::string backup_path = "backup_basic_target.mdbx";
    std::remove(backup_path.c_str());

    {
        Config cfg;
        cfg.pathname = db_path;
        cfg.max_dbs = 8;
        cfg.no_subdir = true;

        auto conn = Connection::create(cfg);
        KeyValueTable<int, std::string> kv(conn, "kv_demo");
        for (int i = 0; i < 100; ++i) {
            kv.insert_or_assign(i, "value_" + std::to_string(i));
        }
        conn->sync_to_disk();

        BackupOptions opt;
        opt.mode = BackupMode::Compact;
        conn->backup_to(backup_path, opt);

        std::cout << "backup created: " << backup_path << std::endl;
    }

    {
        Config cfg;
        cfg.pathname = backup_path;
        cfg.read_only = true;
        cfg.no_subdir = true;

        auto conn = Connection::create(cfg);
        KeyValueTable<int, std::string> kv(conn, "kv_demo");
        std::map<int, std::string> data = kv.retrieve_all();
        std::cout << "verified " << data.size() << " records in backup" << std::endl;
        if (data.size() != 100u) return 1;
        if (data.at(42) != "value_42") return 2;
    }

    std::remove(db_path.c_str());
    std::remove(backup_path.c_str());
    return 0;
}