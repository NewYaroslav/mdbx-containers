# MDBX-Containers

[![MIT License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE) ![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue) ![C++ Standard](https://img.shields.io/badge/C++-11--17-orange) [![CI Windows](https://img.shields.io/github/actions/workflow/status/NewYaroslav/mdbx-containers/ci.yml?branch=main&label=Windows&logo=windows)](https://github.com/NewYaroslav/mdbx-containers/actions/workflows/ci.yml) [![CI Linux](https://img.shields.io/github/actions/workflow/status/NewYaroslav/mdbx-containers/ci.yml?branch=main&label=Linux&logo=linux)](https://github.com/NewYaroslav/mdbx-containers/actions/workflows/ci.yml) [![CI macOS](https://img.shields.io/github/actions/workflow/status/NewYaroslav/mdbx-containers/ci.yml?branch=main&label=macOS&logo=apple)](https://github.com/NewYaroslav/mdbx-containers/actions/workflows/ci.yml)

[Russian version](README-RU.md)

**mdbx-containers** is a lightweight header-only C++11/17 library that bridges [libmdbx](https://github.com/erthink/libmdbx) with familiar STL-style APIs. It persists key-value data in MDBX while providing high performance and transaction helpers.

> Note  
> This project values technical merit over personal views of its authors.  
> See [PHILOSOPHY.md](PHILOSOPHY.md) for details.

## ⚙️ Features

### 🧱 Table APIs
- `KeyValueTable<K, V>` is the main implemented table: one value per key with `insert`, `insert_or_assign`, `find`, `erase`, `clear`, `load`, `reconcile`, `operator[]`, and related helpers.
- `ValueTable<V>` stores one strongly typed singleton value per named table for metadata, module state, snapshots, and single-object configuration records.
- `AnyValueTable<K>` stores heterogeneous values by caller-selected type and supports typed `set`, `insert`, `get`, `find`, `get_or`, `update`, `contains`, `erase`, and `keys`.
- `KeyTable<K>` stores unique keys with a `std::set`-like API: `insert`, `contains`, `erase`, `clear`, `load`, `reconcile`, and related helpers.
- `KeyMultiValueTable<K, V>` stores multiple values per key with a `std::multimap`-like API and preserves repeated identical `(key, value)` pairs.
- `AnyValueTable` type-tag prefix verification is opt-in via `set_type_tag_check(true)` and is disabled by default for compatibility with existing raw records.

### 🔁 Serialization
- Automatic serialization of trivially copyable types.
- Custom types via `to_bytes()` / `from_bytes()`.
- Supports nested STL containers like `std::vector` or `std::list`.

### 🔒 Transactions and Threads
- RAII transactions (`Transaction`).
- Thread-local transaction binding.
- Safe concurrent access through `TransactionTracker` and mutexes.

### 🗄️ Structure & Configuration
- Multiple logical tables inside one MDBX file.
- Flexible configuration: `read_only`, `writemap_mode`, `readahead`, `no_subdir`, `sync_durable`, `max_readers`, `max_dbs`, `relative_to_exe`.
- See `docs/configuration.dox` for details.

### 🧰 Compatibility
- Header-only usage.
- Depends only on [libmdbx](https://github.com/erthink/libmdbx).
- Requires C++11 or later.
- **Windows (MSVC)**: not supported yet. Use MinGW-w64 (GCC) or Clang on Windows.

## 🛠️ Installation

1. Copy the `include/` directory into your project or add this repository as a submodule.
2. Ensure `libmdbx` is available to your build system. Set `MDBXC_DEPS_MODE=BUNDLED` to use the bundled submodule at `external/libmdbx`, or use `SYSTEM`/`AUTO` for an installed package.
3. Use a C++11 (or later) compiler.

### Build with CMake

```bash
cmake -S . -B build \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=ON \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DMDBXC_USE_ASAN=ON \
    -DCMAKE_CXX_STANDARD=17
cmake --build build
ctest --test-dir build --output-on-failure
```

> **Warning**
> Compile every translation unit that uses mdbx-containers with the same C++
> language standard, structure packing, and feature macro configuration. Mixing
> C++11 and C++17 builds, or changing ABI-impacting defines between files, can
> lead to ODR violations and undefined behavior.

Windows users can run the provided `.bat` scripts such as `build-mingw-17-examples.bat`, `build-mingw-17-tests.bat`, or `build-mingw-11-tests.bat`.

## 🧪 Usage Examples

### Basic key-value table

```cpp
#include <mdbx_containers/KeyValueTable.hpp>
#include <iostream>
#include <map>

int main() {
    mdbxc::Config config;
    config.pathname = "example.mdbx";
    config.max_dbs = 4;

    auto conn = mdbxc::Connection::create(config);
    mdbxc::KeyValueTable<int, std::string> table(conn, "my_map");

    table.insert_or_assign(1, "Hello");
    table.insert_or_assign(2, "World");

    std::map<int, std::string> result;
    table.load(result);

    for (const auto& pair : result)
        std::cout << pair.first << ": " << pair.second << "\n";

    return 0;
}
```

### Key-only table

```cpp
#include <mdbx_containers/KeyTable.hpp>
#include <set>

mdbxc::KeyTable<std::string> keys(conn, "tags");
keys.insert("active");
keys.insert("archived");

std::set<std::string> restored = keys.retrieve_all();
```

### Single-value table

```cpp
#include <mdbx_containers/ValueTable.hpp>

struct AppState {
    int schema_version = 1;
    int active_profiles = 0;

    std::vector<uint8_t> to_bytes() const;
    static AppState from_bytes(const void* data, size_t size);
};

mdbxc::ValueTable<AppState> state(conn, "app_state");
state.set(AppState{});

AppState loaded = state.get_or(AppState{});
```

### Multi-value table

```cpp
#include <mdbx_containers/KeyMultiValueTable.hpp>

mdbxc::KeyMultiValueTable<int, std::string> events(conn, "events");
events.insert(7, "created");
events.insert(7, "created"); // exact repeats are preserved
events.insert(7, "sent");

std::vector<std::string> values = events.find(7);
```

### Manual transaction

```cpp
mdbxc::Config config;
config.pathname = "txn.mdbx";
auto conn = mdbxc::Connection::create(config);
mdbxc::KeyValueTable<int, std::string> table(conn, "demo");

conn->begin(mdbxc::TransactionMode::WRITABLE);
table.insert_or_assign(10, "ten");
conn->commit();
```

### Custom struct serialization

```cpp
struct MyData {
    int id;
    double value;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(MyData));
        std::memcpy(bytes.data(), this, sizeof(MyData));
        return bytes;
    }

    static MyData from_bytes(const void* data, size_t size) {
        MyData out{};
        std::memcpy(&out, data, sizeof(MyData));
        return out;
    }
};

mdbxc::KeyValueTable<int, MyData> table(conn, "my_data");
table.insert_or_assign(42, MyData{42, 3.14});
```

## 📚 Documentation

- See the `examples/` directory for more examples.
- API and architecture information lives in the Doxygen source pages under `docs/*.dox`.
- Documentation can be generated with Doxygen; generated `docs/html/` and `docs/latex/` output should not be edited manually.

## 📄 License

This project is licensed under the MIT License.

It can bundle [libmdbx](https://github.com/erthink/libmdbx) from `external/libmdbx`, released under the Apache License 2.0. See `docs/libmdbx.LICENSE` for details.
