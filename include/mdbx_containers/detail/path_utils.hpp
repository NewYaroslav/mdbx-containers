#pragma once
#ifndef _MDBX_CONTAINERS_PATH_UTILS_HPP_INCLUDED
#define _MDBX_CONTAINERS_PATH_UTILS_HPP_INCLUDED

/// \file path_utils.hpp
/// \ingroup mdbxc_utils
/// \brief Utility functions for path manipulation, including relative path computation.

#include <string>
#include <vector>
#include <stdexcept>
#if __cplusplus >= 201703L
#include <filesystem>
#else
#include <cctype>
#endif

#ifdef _WIN32
// For Windows systems
#include <direct.h>
#include <windows.h>
#if __cplusplus < 202002L
#include <locale>
#include <codecvt>
#endif
#include <errno.h>
#else
// For POSIX systems
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#endif

namespace mdbxc {
#   if __cplusplus >= 201703L
    namespace fs = std::filesystem;
#   endif

#if __cplusplus >= 202002L
    /// \brief Converts a UTF-8 string with char8_t characters to std::string.
    inline std::string u8string_to_string(const std::u8string& s) {
        return std::string(s.begin(), s.end());
    }
#endif

    /// \brief Check if path starts with explicit relative prefix.
    /// \param s Path string to inspect.
    /// \return true if path starts with "./", "../", ".\\", or "..\\".
    inline bool is_explicitly_relative(const std::string& s) noexcept {
        auto sw = [&](const char* p){ return s.rfind(p, 0) == 0; };
        return sw("./") || sw("../") || sw(".\\") || sw("..\\");
    }

    /// \brief Checks whether the given path is absolute (cross-platform).
    /// \param path File or directory path.
    /// \return True if path is absolute, false otherwise.
    inline bool is_absolute_path(const std::string& path) {
#       if __cplusplus >= 201703L
        return fs::u8path(path).is_absolute();
#       else
#       ifdef _WIN32
        // On Windows: absolute path starts with drive letter or UNC path (\\)
        return (path.size() >= 2 && std::isalpha(path[0]) && path[1] == ':') || 
               (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') ||
               (path.size() >= 2 && path[0] == '/'  && path[1] == '/');
#       else
        // On POSIX: absolute path starts with /
        return !path.empty() && path[0] == '/';
#       endif
#       endif
    }

    /// \brief Extracts the parent directory from a full file path.
    /// \param file_path Path to a file (e.g., "data/testdb").
    /// \return Directory path (e.g., "data")
    inline std::string get_parent_path(const std::string& file_path) {
#       if __cplusplus >= 201703L
#       if __cplusplus >= 202002L
        auto parent = fs::u8path(file_path).parent_path().u8string();
        return u8string_to_string(parent);
#       else
        return fs::u8path(file_path).parent_path().u8string();
#       endif
#       else
        size_t pos = file_path.find_last_of("/\\");
        if (pos == std::string::npos)
            return "."; // current dir
        return file_path.substr(0, pos);
#       endif
    }

    /// \brief Retrieves the directory of the executable file.
    /// \return A string containing the directory path of the executable.
    inline std::string get_exec_dir() {
#       ifdef _WIN32
        std::vector<wchar_t> buffer(MAX_PATH);
        HMODULE hModule = GetModuleHandle(NULL);

        // Try to get the path
        std::size_t size = static_cast<std::size_t>(GetModuleFileNameW(hModule, buffer.data(), static_cast<DWORD>(buffer.size())));

        // If the path is too long, increase the buffer size
        while (size == buffer.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            buffer.resize(buffer.size() * 2);  // Double the buffer size
            size = static_cast<std::size_t>(GetModuleFileNameW(hModule, buffer.data(), static_cast<DWORD>(buffer.size())));
        }

        if (size == 0) {
            throw std::runtime_error("Failed to get executable path.");
        }

        std::wstring exe_path(buffer.begin(), buffer.begin() + size);

        // Trim the path to the directory (remove the file name, keep only the folder path)
        size_t pos = exe_path.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            exe_path = exe_path.substr(0, pos);
        }

#       if __cplusplus >= 202002L
        fs::path path_wide = exe_path;
        auto tmp = path_wide.u8string();
        return u8string_to_string(tmp);
#       else
        // Convert from std::wstring (UTF-16) to std::string (UTF-8)
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        return converter.to_bytes(exe_path);
#       endif

#       else
        char result[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);

        if (count == -1) {
            throw std::runtime_error("Failed to get executable path.");
        }

        std::string exe_path(result, count);

        // Trim the path to the directory (remove the file name, keep only the folder path)
        size_t pos = exe_path.find_last_of("\\/");
        if (pos != std::string::npos) {
            exe_path = exe_path.substr(0, pos);
        }

        return exe_path;
#       endif
    }

    /// \brief Extracts the file name from a full file path.
    /// \param file_path The full file path as a string.
    /// \return The extracted file name, or the full string if no directory separator is found.
    inline std::string get_file_name(const std::string& file_path) {
#       if __cplusplus >= 201703L
#       if __cplusplus >= 202002L
        auto name = fs::u8path(file_path).filename().u8string();
        return u8string_to_string(name);
#       else
        return fs::u8path(file_path).filename().u8string();
#       endif
#       else
        size_t pos = file_path.find_last_of("/\\");
        if (pos == std::string::npos) return file_path;
        return file_path.substr(pos + 1);
#       endif
    }

    /// \brief Converts a UTF-8 string to an ANSI string (Windows-specific).
    /// \param utf8 The UTF-8 encoded string.
    /// \return The converted ANSI string.
    inline std::string utf8_to_ansi(const std::string& utf8) noexcept {
#ifdef _WIN32
        int n_len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
        if (n_len == 0) return {};

        std::wstring wide_string(n_len + 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide_string[0], n_len);

        n_len = WideCharToMultiByte(CP_ACP, 0, wide_string.c_str(), -1, NULL, 0, NULL, NULL);
        if (n_len == 0) return {};

        std::string ansi_string(n_len - 1, '\0');
        WideCharToMultiByte(CP_ACP, 0, wide_string.c_str(), -1, &ansi_string[0], n_len, NULL, NULL);
        return ansi_string;
#else
        return utf8;
#endif
    }

#if __cplusplus >= 201703L

    /// \brief Computes the relative path from base_path to file_path using C++17 std::filesystem.
    /// \param file_path The target file path.
    /// \param base_path The base path from which to compute the relative path.
    /// \return A string representing the relative path from base_path to file_path.
    inline std::string make_relative(const std::string& file_path, const std::string& base_path) {
        if (base_path.empty()) return file_path;
        fs::path fileP = fs::u8path(file_path);
        fs::path baseP = fs::u8path(base_path);
        std::error_code ec; // For exception-safe operation
        fs::path relativeP = fs::relative(fileP, baseP, ec);
        if (ec) {
            // If there is an error, return the original file_path
            return file_path;
        } else {
#       if __cplusplus >= 202002L
            return u8string_to_string(relativeP.u8string());
#       else
            return relativeP.u8string();
#       endif
        }
    }

    /// \brief Creates directories recursively for the given path using C++17 std::filesystem.
    /// \param path The directory path to create.
    /// \throws std::runtime_error if the directories cannot be created.
      inline void create_directories(const std::string& path) {
#   ifdef _WIN32
#       if __cplusplus >= 202002L
        fs::path parent_dir = fs::u8path(get_parent_path(path));
#       else
        // Convert UTF-8 string to wide string for Windows
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        std::wstring wide_path = converter.from_bytes(get_parent_path(path));
        fs::path parent_dir = fs::path(wide_path);
#       endif
#   else
        fs::path parent_dir = fs::u8path(get_parent_path(path));
#   endif
        if (parent_dir.empty()) parent_dir = fs::current_path();
        if (!fs::exists(parent_dir)) {
            std::error_code ec;
            if (!std::filesystem::create_directories(parent_dir, ec)) {
#       if __cplusplus >= 202002L
                auto p = parent_dir.u8string();
                throw std::runtime_error("Failed to create directories for path: " + u8string_to_string(p));
#       else
                throw std::runtime_error("Failed to create directories for path: " + parent_dir.u8string());
#       endif
            }
        }
    }

#else
    // С++11/14

    /// \brief Check if character is a path separator.
    /// \param c Character to check.
    /// \return true if \a c is '/' or '\\'.
    inline bool is_path_sep(char c) {
        return c == '/' || c == '\\';
    }

    /// \brief Normalize path removing '.' and '..' components.
    /// \param in Path to normalize.
    /// \return Normalized path.
    inline std::string lexically_normal_compat(const std::string& in) {
#       ifdef _WIN32
        const char SEP = '\\';
#       else
        const char SEP = '/';
#       endif
        const char* s = in.c_str();
        const size_t n = in.size();
        size_t i = 0;

        // ----- parse prefix (root) -----
#       ifdef _WIN32
        bool is_unc = false;
        std::string drive;          // "C:"
        std::vector<std::string> root_parts; // for UNC: server, share

        // UNC: \\server\share\...
        if (n >= 2 && is_path_sep(s[0]) && is_path_sep(s[1])) {
            is_unc = true;
            i = 2;
            // server
            size_t start = i;
            while (i < n && !is_path_sep(s[i])) ++i;
            if (i > start) root_parts.push_back(in.substr(start, i - start));
            if (i < n && is_path_sep(s[i])) ++i;
            // share
            start = i;
            while (i < n && !is_path_sep(s[i])) ++i;
            if (i > start) root_parts.push_back(in.substr(start, i - start));
            // пропускаем дополнительные слеши
            while (i < n && is_path_sep(s[i])) ++i;
        }
        // Drive: "X:" (возможно абсолютный "X:\..." или относительный "X:foo")
        else if (n >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
            drive.assign(in, 0, 2);
            i = 2;
            // пропустим один/несколько разделителей, если есть
            while (i < n && is_path_sep(s[i])) ++i;
        }
        const bool is_abs_root = is_unc || (!drive.empty() && (n >= 3 && is_path_sep(s[2]))) ||
                                 (!drive.size() && n >= 1 && is_path_sep(s[0]));
#       else
        const bool is_abs_root = (n >= 1 && s[0] == '/');
        if (is_abs_root) i = 1;
#       endif

        // ----- tokenize and fold . / .. -----
        std::vector<std::string> comps;

        auto push_component = [&](const std::string& token) {
            if (token.empty() || token == ".") return;
            if (token == "..") {
#           ifdef _WIN32
            // Для UNC «корень» — server/share: их нельзя выталкивать назад.
            size_t protected_root = 0;
            if (
                    // UNC и есть server/share
                    true
#               ifdef _WIN32
                    && false
#               endif
                ) { /* защищённая зона настроена ниже */ }
#           endif
                if (!comps.empty() && comps.back() != "..") {
                    // нельзя уходить выше корня абсолютного пути
                    comps.pop_back();
                } else {
                    // относительный путь или уже нет куда сворачивать
                    comps.push_back("..");
                }
            } else {
                comps.push_back(token);
            }
        };

#       ifdef _WIN32
        // Для UNC защищаем первые два компонента (server/share) от свёртки ".."
        size_t protected_count = 0;
        if (is_unc) {
            for (size_t k = 0; k < root_parts.size(); ++k) {
                comps.push_back(root_parts[k]);
            }
            protected_count = comps.size();
        }
#       endif

        while (i < n) {
            // пропускаем повторные разделители
            while (i < n && is_path_sep(s[i])) ++i;
            if (i >= n) break;
            size_t start = i;
            while (i < n && !is_path_sep(s[i])) ++i;
            std::string token = in.substr(start, i - start);

            if (token == "..") {
#               ifdef _WIN32
                if (!comps.empty() && comps.size() > protected_count && comps.back() != "..") {
                    comps.pop_back();
                } else if (!is_abs_root || (is_unc && comps.size() > protected_count)) {
                    comps.push_back("..");
                }
#               else
                if (is_abs_root) {
                    if (!comps.empty() && comps.back() != "..") comps.pop_back();
                } else {
                    comps.push_back("..");
                }
#               endif
            } else if (token != ".") {
                comps.push_back(token);
            }
        }

        // ----- rebuild -----
        std::string out;

#       ifdef _WIN32
        if (is_unc) {
            out = "\\\\";
            for (size_t k = 0; k < comps.size(); ++k) {
                if (k) out += SEP;
                out += comps[k];
            }
            return out;
        }
        if (!drive.empty()) {
            // Абсолютный drive-root → "X:\", drive-relative → "X:"
            bool drive_absolute = (n >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1]==':' && is_path_sep(s[2]));
            out = drive;
            if (drive_absolute) out += SEP;
        } else if (is_abs_root) {
            out.push_back(SEP);
        }
#       else
        if (is_abs_root) out.push_back(SEP);
#       endif

        for (size_t k = 0; k < comps.size(); ++k) {
            if (!out.empty() && out.back() != SEP) out.push_back(SEP);
            out += comps[k];
        }

        if (out.empty()) {
#           ifdef _WIN32
            out = !drive.empty() ? drive : std::string(".");
#           else
            out = is_abs_root ? std::string(1, SEP) : std::string(".");
#           endif
        }
        return out;
    }

    /// \struct PathComponents
    /// \brief Structure to hold the root and components of a path.
    struct PathComponents {
        std::string root;                       ///< The root part of the path (e.g., "/", "C:")
        std::vector<std::string> components;    ///< The components of the path.
    };

    /// \brief Splits a path into its root and components.
    /// \param path The path to split.
    /// \return A PathComponents object containing the root and components of the path.
      inline PathComponents split_path(const std::string& path) {
        PathComponents result;
        size_t i = 0;
        size_t n = path.size();

        // Handle root paths for Unix and Windows
        if (n >= 1 && (path[0] == '/' || path[0] == '\\')) {
            // Unix root "/"
            result.root = "/";
            ++i;
        } else if (n >= 2 && std::isalpha(path[0]) && path[1] == ':') {
            // Windows drive letter "C:"
            result.root = path.substr(0, 2);
            i = 2;
            if (n >= 3 && (path[2] == '/' || path[2] == '\\')) {
                // "C:/"
                ++i;
            }
        }

        // Split the path into components
        while (i < n) {
            // Skip path separators
            while (i < n && (path[i] == '/' || path[i] == '\\')) {
                ++i;
            }
            // Find the next separator
            size_t j = i;
            while (j < n && path[j] != '/' && path[j] != '\\') {
                ++j;
            }
            if (i < j) {
                result.components.push_back(path.substr(i, j - i));
                i = j;
            }
        }

        return result;
    }
    
    /// \brief Creates directories recursively for the given path.
    /// \param path The directory path to create.
    /// \throws std::runtime_error if the directories cannot be created.
      inline void create_directories(const std::string& path) {
        if (path.empty()) return;
        PathComponents path_pc = split_path(get_parent_path(path));
        auto &components = path_pc.components;
        size_t components_size = components.size();

        // Build the path incrementally and create directories
        std::string current_path = path_pc.root;
        for (size_t i = 0; i < components_size; ++i) {
            if (!current_path.empty() && current_path.back() != '/' && current_path.back() != '\\') {
                current_path += '/';
            }
            current_path += components[i];

            // Skip special components
            if (components[i] == ".." ||
                components[i] == "/" ||
                components[i] == "~/") continue;
#           ifdef _WIN32
            int ret = _mkdir(utf8_to_ansi(current_path).c_str());
#           else
            int ret = mkdir(current_path.c_str(), 0755);
#           endif
            int errnum = errno;
            if (ret != 0 && errnum != EEXIST) {
                throw std::runtime_error("Failed to create directory: " + current_path);
            }
        }
    }

#endif // __cplusplus >= 201703L

}; // namespace mdbxc

#endif // _MDBX_CONTAINERS_PATH_UTILS_HPP_INCLUDED
