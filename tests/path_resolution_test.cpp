// file: tests/path_resolution_test.cpp
#if __cplusplus < 201703L
#include <iostream>
int main() {
    std::cout << "path_resolution_test skipped for C++11" << std::endl;
    return 0;
}
#else
// build (пример):
//   g++ -std=gnu++17 -O2 -Wall -Wextra -Iinclude \
//       tests/path_resolution_test.cpp -o path_resolution_test \
//       -lmd b x (или нужные вам библиотеки)
//
// Тестирует политику разрешения путей и работу no_subdir.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <filesystem>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
#endif

#include <mdbx_containers/KeyValueTable.hpp>

// ----------------------- helpers -----------------------
namespace fs = std::filesystem;

static std::string exe_dir() {
#ifdef _WIN32
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n == buf.size()) throw std::runtime_error("GetModuleFileNameW failed");
    fs::path p(buf.data(), buf.data() + n);
    return p.parent_path().u8string();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) throw std::runtime_error("_NSGetExecutablePath failed");
    fs::path p = fs::weakly_canonical(fs::path(path.data()));
    return p.parent_path().u8string();
#else
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path)-1);
    if (n <= 0) throw std::runtime_error("readlink /proc/self/exe failed");
    path[n] = '\0';
    fs::path p = fs::weakly_canonical(fs::path(path));
    return p.parent_path().u8string();
#endif
}

static std::string uniq_suffix() {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(static_cast<uint64_t>(now));
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t x = dist(rng);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(x));
    return std::string(buf);
}

static bool dir_nonempty(const fs::path& p) {
    if (!fs::exists(p) || !fs::is_directory(p)) return false;
    return fs::directory_iterator(p) != fs::directory_iterator{};
}

// Правило разрешения пути, которое должна соблюдать библиотека.
static fs::path expected_path_from_policy(const std::string& pathname,
                                          bool relative_to_exe,
                                          const fs::path& exeDir,
                                          const fs::path& cwd) {
    auto explicitly_rel = [](const std::string& s){
        auto sw = [&](const char* p){ return s.rfind(p, 0) == 0; };
        return sw("./") || sw("../") || sw(".\\") || sw("..\\");
    };
    fs::path p = fs::u8path(pathname);
    if (p.is_absolute()) {
        // как есть
    } else if (explicitly_rel(pathname)) {
        p = cwd / p;
    } else if (relative_to_exe) {
        p = exeDir / p;
    } else {
        p = cwd / p;
    }
#if __cplusplus >= 202002L
    p = fs::weakly_canonical(p);
#else
    p = p.lexically_normal();
#endif
    return p;
}

template <typename DoWriteVerify>
static void run_case(const std::string& case_name,
                     const std::string& raw_pathname,
                     bool relative_to_exe,
                     bool no_subdir,
                     const fs::path& exeDir,
                     const fs::path& cwd,
                     DoWriteVerify&& doWriteVerify) {
    // ожидаемое место БД
    fs::path expect = expected_path_from_policy(raw_pathname, relative_to_exe, exeDir, cwd);

    // сконфигурировать и создать соединение
    mdbxc::Config cfg;
    cfg.pathname        = raw_pathname;
    cfg.max_dbs         = 14;
    cfg.no_subdir       = no_subdir;
    cfg.relative_to_exe = relative_to_exe;
    cfg.relative_to_exe = relative_to_exe;

    auto conn = mdbxc::Connection::create(cfg);

    // реальное использование БД: таблица и запись/чтение
    {
        // имя таблицы тоже уникализируем
        std::string tbl = "kv_i8_i8_" + case_name + "_" + uniq_suffix();
        mdbxc::KeyValueTable<int8_t, int8_t> kv(conn, tbl);
        doWriteVerify(kv); // пользовательская запись и проверка
    }

    // проверки на ФС
    if (!no_subdir) {
        // директория БД должна существовать и быть не пустой
        assert(fs::exists(expect));
        assert(fs::is_directory(expect));
        assert(dir_nonempty(expect));
    } else {
        // файл БД должен существовать (lock-файл может быть рядом)
        assert(fs::exists(expect));
        assert(fs::is_regular_file(expect));
    }
}

// ----------------------- main -----------------------
int main() try {
    // 1) exeDir (фиксированный), 2) staging CWD (меняем на уникальную временную)
    const fs::path exeDir = fs::path(exe_dir());
    const fs::path baseTmp = fs::temp_directory_path() / ("mdbxc_path_tests_" + uniq_suffix());
    fs::create_directories(baseTmp);

    // Сделаем CWD гарантированно другим, чем exeDir
    const fs::path cwdA = baseTmp / "cwdA";
    const fs::path cwdB = baseTmp / "cwdB";
    fs::create_directories(cwdA);
    fs::create_directories(cwdB);

    // ---- Группа 1: no_subdir = false ----
    fs::current_path(cwdA);

    // 1.1 relative_to_exe = false → CWD
    run_case("dir_rel_cwd",
             "data/db_dir_cwd_" + uniq_suffix(),
             /*relative_to_exe=*/false,
             /*no_subdir=*/false,
             exeDir, fs::current_path(),
             [](auto& kv){
                 kv.insert_or_assign(1, (int8_t)42);
#if __cplusplus >= 201703L
                 auto v = kv.find(1);
                 assert(v && *v == (int8_t)42);
#else
                 auto v = kv.find_compat(1);
                 assert(v.first && v.second == (int8_t)42);
#endif
             });

    // 1.2 relative_to_exe = true → exeDir
    run_case("dir_rel_exe",
             "data/db_dir_exe_" + uniq_suffix(),
             /*relative_to_exe=*/true,
             /*no_subdir=*/false,
             exeDir, fs::current_path(),
             [](auto& kv){
                 kv.insert_or_assign(1, (int8_t)7);
#if __cplusplus >= 201703L
                 auto v = kv.find(1);
                 assert(v && *v == (int8_t)7);
#else
                 auto v = kv.find_compat(1);
                 assert(v.first && v.second == (int8_t)7);
#endif
             });

    // 1.3 Явно относительный "./..." → игнорируем relative_to_exe, берём CWD
    run_case("dir_explicit_cwd",
             "./data/db_dir_explicit_" + uniq_suffix(),
             /*relative_to_exe=*/true,     // флаг выставлен, но должен быть проигнорирован
             /*no_subdir=*/false,
             exeDir, fs::current_path(),
             [](auto& kv){
                 kv.insert_or_assign(1, (int8_t)-5);
#if __cplusplus >= 201703L
                 auto v = kv.find(1);
                 assert(v && *v == (int8_t)-5);
#else
                 auto v = kv.find_compat(1);
                 assert(v.first && v.second == (int8_t)-5);
#endif
             });

    // ---- Группа 2: no_subdir = true (файловый режим) ----
    fs::current_path(cwdB);

    // 2.1 relative_to_exe = false → CWD
    run_case("file_rel_cwd",
             "data/db_file_cwd_" + uniq_suffix(),
             /*relative_to_exe=*/false,
             /*no_subdir=*/true,
             exeDir, fs::current_path(),
             [](auto& kv){
                 kv.insert_or_assign(1, (int8_t)11);
#if __cplusplus >= 201703L
                 auto v = kv.find(1);
                 assert(v && *v == (int8_t)11);
#else
                 auto v = kv.find_compat(1);
                 assert(v.first && v.second == (int8_t)11);
#endif
             });

    // 2.2 relative_to_exe = true → exeDir
    run_case("file_rel_exe",
             "data/db_file_exe_" + uniq_suffix(),
             /*relative_to_exe=*/true,
             /*no_subdir=*/true,
             exeDir, fs::current_path(),
             [](auto& kv){
                 kv.insert_or_assign(1, (int8_t)12);
#if __cplusplus >= 201703L
                 auto v = kv.find(1);
                 assert(v && *v == (int8_t)12);
#else
                 auto v = kv.find_compat(1);
                 assert(v.first && v.second == (int8_t)12);
#endif
             });

    // 2.3 Явно относительный "./..." → CWD
    run_case("file_explicit_cwd",
             "./data/db_file_explicit_" + uniq_suffix(),
             /*relative_to_exe=*/true,     // игнорируем из-за "./"
             /*no_subdir=*/true,
             exeDir, fs::current_path(),
             [](auto& kv){
                 kv.insert_or_assign(1, (int8_t)13);
#if __cplusplus >= 201703L
                 auto v = kv.find(1);
                 assert(v && *v == (int8_t)13);
#else
                 auto v = kv.find_compat(1);
                 assert(v.first && v.second == (int8_t)13);
#endif
             });

    std::cout << "[result] path resolution tests passed\n";
    return 0;
}
catch (const std::exception& e) {
    std::cerr << "[error] " << e.what() << "\n";
    return 1;
}
#endif // __cplusplus < 201703L
