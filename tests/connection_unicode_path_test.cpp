#include "test_assert.hpp"

#include <mdbx_containers/common.hpp>

#include <ctime>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::string unicode_dir_name() {
    return "\xD0\xB1\xD0\xB0\xD0\xB7\xD0\xB0"
           "_\xE6\xB5\x8B\xE8\xAF\x95"
           "_\xF0\x9F\x98\x80";
}

#ifdef _WIN32
std::string temp_dir() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetTempPathW(static_cast<DWORD>(buffer.size()), &buffer[0]);
    MDBXC_TEST_ASSERT(size > 0);
    MDBXC_TEST_ASSERT(size < buffer.size());
    buffer.resize(size);
    return mdbxc::wide_to_utf8(buffer);
}

std::string suffix() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu_%lu",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long>(GetTickCount()));
    return buf;
}

void remove_file_if_exists(const std::string& path) {
    DeleteFileW(mdbxc::utf8_to_wide(path).c_str());
}

void remove_dir_if_exists(const std::string& path) {
    RemoveDirectoryW(mdbxc::utf8_to_wide(path).c_str());
}
#else
std::string temp_dir() {
    return "/tmp/";
}

std::string suffix() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%ld_%ld",
                  static_cast<long>(getpid()),
                  static_cast<long>(std::time(nullptr)));
    return buf;
}

void remove_file_if_exists(const std::string& path) {
    unlink(path.c_str());
}

void remove_dir_if_exists(const std::string& path) {
    rmdir(path.c_str());
}
#endif

void open_and_close(const std::string& db_path) {
    mdbxc::Config config;
    config.pathname = db_path;
    config.relative_to_exe = false;
    config.no_subdir = true;
    config.max_dbs = 2;

    mdbxc::Connection connection;
    connection.connect(config);
    connection.disconnect();
}

} // namespace

int main() {
    const std::string root = temp_dir() + "mdbxc_connection_unicode_" + suffix();
    const std::string unicode_dir = root + "/" + unicode_dir_name();
    const std::string db_path = unicode_dir + "/data.mdbx";

    remove_file_if_exists(db_path);
    remove_file_if_exists(db_path + "-lck");
    remove_dir_if_exists(unicode_dir);
    remove_dir_if_exists(root);

    open_and_close(db_path);
    open_and_close(db_path);

    remove_file_if_exists(db_path);
    remove_file_if_exists(db_path + "-lck");
    remove_dir_if_exists(unicode_dir);
    remove_dir_if_exists(root);

    return 0;
}
