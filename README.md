# mdbx-containers

**mdbx-containers** is a lightweight header-only C++11/17 library that bridges [libmdbx](https://github.com/erthink/libmdbx) with familiar STL containers such as `std::map` and `std::set`. It transparently persists your in-memory data in MDBX while providing high performance and thread-safe transactions.

> **Note:** `KeyTable` and `KeyMultiValueTable` are not implemented yet.

## Features

### Unified API
- Identical interface for all table types: `insert`, `insert_or_assign`, `find`, `erase`, `clear`, `load`, `reconcile`, `operator[]` and more.
- Four container flavours:
  - `KeyTable<K>` – keys only (not implemented yet);
  - `KeyValueTable<K, V>` – one value per key;
  - `KeyMultiValueTable<K, V>` – multiple values per key (`std::multimap`) (not implemented yet);
  - `AnyValueTable<K>` – heterogeneous values with runtime type checks.

### Serialization
- Automatic serialization of trivially copyable types.
- Custom types via `to_bytes()` / `from_bytes()`.
- Supports nested STL containers like `std::vector` or `std::list`.

### Transactions and Threads
- RAII transactions (`Transaction`).
- Thread-local transaction binding.
- Safe concurrent access through `TransactionTracker` and mutexes.

### Structure & Configuration
- Multiple logical tables inside one MDBX file.
- Flexible configuration: `read_only`, `writemap_mode`, `readahead`, `no_subdir`, `sync_durable`, `max_readers`, `max_dbs`, `relative_to_exe`.
- See `docs/configuration.dox` for details.

### Compatibility
- Header-only usage.
- Depends only on [libmdbx](https://github.com/erthink/libmdbx).
- Requires C++11 or later.
- **Windows (MSVC)**: not supported yet. Use MinGW-w64 (GCC) or Clang on Windows.

## Installation

1. Copy the `include/` directory into your project or add this repository as a submodule.
2. Ensure `libmdbx` is available to your build system (set `MDBXC_DEPS_MODE=BUNDLED` to build it automatically).
3. Use a C++11 (or later) compiler.

### Build with CMake

```bash
cmake -S . -B build \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_STATIC_LIB=ON \
    -DMDBXC_BUILD_TESTS=ON \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DMDBXC_USE_ASAN=ON \
    -DCMAKE_CXX_STANDARD=17
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows users can run the provided `.bat` scripts such as `build-mingw-17-examples.bat`, `build-mingw-17-tests.bat`, or `build-mingw-11-tests.bat`.

## Usage Examples

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

## Documentation

- See the `examples/` directory for more examples.
- API and architecture information can be found in the wiki (if available).
- Documentation can be generated with Doxygen.

## License

This project is licensed under the MIT License.

It bundles [libmdbx](https://github.com/erthink/libmdbx) released under the Apache License 2.0. See `docs/libmdbx.LICENSE` for details.
