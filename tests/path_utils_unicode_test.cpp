#include "test_assert.hpp"
#include <mdbx_containers/detail/path_utils.hpp>

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

const char* cyrillic_sample() {
    return "\xD0\xBA\xD0\xB8\xD1\x80\xD0\xB8\xD0\xBB\xD0\xBB\xD0\xB8\xD1\x86\xD0\xB0_\xD0\x81_\xD1\x82\xD0\xB5\xD1\x81\xD1\x82";
}

#ifdef _WIN32
std::wstring wide_cyrillic_sample() {
    return L"\x043A\x0438\x0440\x0438\x043B\x043B\x0438\x0446\x0430_\x0401_\x0442\x0435\x0441\x0442";
}

std::wstring temp_dir() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetTempPathW(static_cast<DWORD>(buffer.size()), &buffer[0]);
    MDBXC_TEST_ASSERT(size > 0);
    MDBXC_TEST_ASSERT(size < buffer.size());
    buffer.resize(size);
    return buffer;
}

std::string suffix() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu_%lu",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long>(GetTickCount()));
    return buf;
}
#endif

} // namespace

int main() {
#ifdef _WIN32
    const std::string utf8 = cyrillic_sample();
    const std::wstring wide = wide_cyrillic_sample();

    MDBXC_TEST_ASSERT(mdbxc::wide_to_utf8(wide) == utf8);
    MDBXC_TEST_ASSERT(mdbxc::utf8_to_wide(utf8) == wide);

    const std::string root = mdbxc::wide_to_utf8(temp_dir()) + "mdbxc_unicode_" + suffix();
    const std::string first_parent = root + "\\" + utf8;
    const std::string second_parent = first_parent + "\\" + utf8 + "_nested";
    const std::string parent = second_parent + "\\" + utf8 + "_leaf";
    const std::string db_path = parent + "\\db.mdbx";

    mdbxc::create_directories(db_path);

    const std::wstring wide_parent = mdbxc::utf8_to_wide(parent);
    DWORD attributes = GetFileAttributesW(wide_parent.c_str());
    MDBXC_TEST_ASSERT(attributes != INVALID_FILE_ATTRIBUTES);
    MDBXC_TEST_ASSERT((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);

    RemoveDirectoryW(wide_parent.c_str());
    RemoveDirectoryW(mdbxc::utf8_to_wide(second_parent).c_str());
    RemoveDirectoryW(mdbxc::utf8_to_wide(first_parent).c_str());
    RemoveDirectoryW(mdbxc::utf8_to_wide(root).c_str());
#else
    const std::string path = std::string("tmp/") + cyrillic_sample() + "/db.mdbx";
    MDBXC_TEST_ASSERT(mdbxc::get_parent_path(path) == std::string("tmp/") + cyrillic_sample());
    MDBXC_TEST_ASSERT(mdbxc::get_file_name(path) == "db.mdbx");
#endif

    return 0;
}
