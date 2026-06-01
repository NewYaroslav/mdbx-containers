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

const char* ukrainian_sample() {
    return "\xD1\x83\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0_\xD0\x84_\xD0\x87_\xD0\x86_\xD2\x90";
}

const char* exotic_sample() {
    return "\xE0\xBA\x9E\xE0\xBA\xB2\xE0\xBA\xAA\xE0\xBA\xB2\xE0\xBA\xA5\xE0\xBA\xB2\xE0\xBA\xA7";
}

#ifdef _WIN32
std::wstring wide_cyrillic_sample() {
    return L"\x043A\x0438\x0440\x0438\x043B\x043B\x0438\x0446\x0430_\x0401_\x0442\x0435\x0441\x0442";
}

std::wstring wide_ukrainian_sample() {
    return L"\x0443\x043A\x0440\x0430\x0457\x043D\x0441\x044C\x043A\x0430_\x0404_\x0407_\x0406_\x0490";
}

std::wstring wide_exotic_sample() {
    return L"\x0E9E\x0EB2\x0EAA\x0EB2\x0EA5\x0EB2\x0EA7";
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
    const std::string cyrillic = cyrillic_sample();
    const std::string ukrainian = ukrainian_sample();
    const std::string exotic = exotic_sample();

    MDBXC_TEST_ASSERT(mdbxc::wide_to_utf8(wide_cyrillic_sample()) == cyrillic);
    MDBXC_TEST_ASSERT(mdbxc::utf8_to_wide(cyrillic) == wide_cyrillic_sample());
    MDBXC_TEST_ASSERT(mdbxc::wide_to_utf8(wide_ukrainian_sample()) == ukrainian);
    MDBXC_TEST_ASSERT(mdbxc::utf8_to_wide(ukrainian) == wide_ukrainian_sample());
    MDBXC_TEST_ASSERT(mdbxc::wide_to_utf8(wide_exotic_sample()) == exotic);
    MDBXC_TEST_ASSERT(mdbxc::utf8_to_wide(exotic) == wide_exotic_sample());

    bool failed = false;
    try {
        (void)mdbxc::utf8_to_wide(std::string("\xD0", 1));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    MDBXC_TEST_ASSERT(failed);

    const std::string root = mdbxc::wide_to_utf8(temp_dir()) + "mdbxc_unicode_" + suffix();
    const std::string first_parent = root + "\\" + cyrillic;
    const std::string second_parent = first_parent + "\\" + ukrainian;
    const std::string parent = second_parent + "\\" + exotic;
    const std::string db_path = parent + "\\db.mdbx";

    MDBXC_TEST_ASSERT(mdbxc::get_file_name(db_path) == "db.mdbx");
    MDBXC_TEST_ASSERT(mdbxc::get_parent_path(db_path) == parent);

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
    const std::string path = std::string("tmp/") + cyrillic_sample() + "/" +
                             ukrainian_sample() + "/" + exotic_sample() + "/db.mdbx";
    const std::string parent = std::string("tmp/") + cyrillic_sample() + "/" +
                               ukrainian_sample() + "/" + exotic_sample();
    MDBXC_TEST_ASSERT(mdbxc::get_parent_path(path) == parent);
    MDBXC_TEST_ASSERT(mdbxc::get_file_name(path) == "db.mdbx");
#endif

    return 0;
}
