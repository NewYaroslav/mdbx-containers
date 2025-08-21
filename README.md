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
2. Ensure `libmdbx` is available to your build system (it can be built automatically with `BUILD_DEPS=ON`).
3. Use a C++11 (or later) compiler.

### Build with CMake

```bash
cmake -S . -B build \
    -DBUILD_DEPS=ON \
    -DBUILD_STATIC_LIB=ON \
    -DBUILD_TESTS=ON \
    -DBUILD_EXAMPLES=ON
cmake --build build
```

Windows users can run the provided `.bat` scripts such as `build-17-examples.bat` or `build-mingw-17-tests.bat`.

## Example

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

    // Write
    table.insert_or_assign(1, "Hello");
    table.insert_or_assign(2, "World");

    // Read
    std::map<int, std::string> result;
    table.load(result);

    for (const auto& pair : result)
        std::cout << pair.first << ": " << pair.second << "\n";

    return 0;
}
```

## Documentation

- See the `examples/` directory for more examples.
- API and architecture information can be found in the wiki (if available).
- Documentation can be generated with Doxygen.

## License

This project is licensed under the MIT License.

It bundles [libmdbx](https://github.com/erthink/libmdbx) released under the Apache License 2.0. See `docs/libmdbx.LICENSE` for details.
