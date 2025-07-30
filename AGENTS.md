# AGENTS.md

# Overview

**mdbx-containers** is a lightweight, header-only C++11/17 library that bridges standard STL containers (`std::map`, `std::vector`, `std::set`, `std::unordered_map`, etc.) with [libmdbx](https://github.com/erthink/libmdbx), providing high-performance, transactional key-value storage with a clean and familiar interface.

It is designed for developers who want to:

- Persist STL containers in a crash-safe, transactional key-value database
- Use `std` containers as intuitive views over durable MDBX-backed data
- Avoid writing manual serialization logic or low-level MDBX boilerplate
- Share a single MDBX file among multiple logical tables (sub-databases)
- Build high-performance systems with thread-safe and efficient access patterns

# Features

- Transparent persistence of `std::map`, `std::set`, `std::vector`, `std::unordered_map`, and similar containers
- Key-only, key-value, and multimap-like support using `KeyTable`, `KeyValueTable`, and `KeyMultiValueTable`
- Serialization for trivially-copyable types via `memcpy`, or custom types via `to_bytes()` / `from_bytes()`
- Automatic transaction management using RAII and per-thread `TransactionTracker`
- Multiple logical tables in one environment via named `MDBX_dbi`
- Thread-safe use across concurrent readers/writers

## Use Cases

- Persistent key-value mappings:  
  `KeyValueTable<int, std::string>`, `KeyValueTable<std::string, double>`

- Multimap-like storage:  
  `KeyMultiValueTable<std::string, int>` to associate multiple values with a single key

- Set-like persistence:  
  `KeyTable<uint32_t>` for fast and compact storage of unique keys

- Nested STL containers:  
  `KeyValueTable<std::string, std::vector<SimpleStruct>>` for bulk object serialization

- Transparent support for PODs and trivially copyable types:  
  Automatically serialized via `memcpy`

- Custom types with explicit serialization:  
  Types implementing `to_bytes()` / `from_bytes()` are serialized automatically

- Multiple logical tables:  
  One `Connection` can manage multiple `KeyValueTable` instances, each with a distinct `table_name`

- Real dataset processing with low overhead:  
  Minimal boilerplate to test storage, iteration, reconciliation, etc.

## Installation & Build

You can use **mdbx-containers** as a header-only library or build it as a static library.

### Requirements

- CMake 3.18+
- C++11 or later
- [libmdbx](https://github.com/erthink/libmdbx) (automatically built if `BUILD_DEPS=ON`)

### Using as a Submodule

```bash
git submodule add https://github.com/NewYaroslav/mdbx-containers.git
```

### Build with CMake

Header-only usage (no build step needed):

```bash
# Just add `include/` to your include path and link with libmdbx.
```

Or build the static library (optional):

```bash
cmake -S . -B build \
    -DBUILD_DEPS=ON \
    -DBUILD_STATIC_LIB=ON \
    -DBUILD_TESTS=ON \
    -DBUILD_EXAMPLES=ON

cmake --build build
```

To run tests:

```bash
cd build
ctest --output-on-failure
```

### CMake Options

| Option               | Default | Description                                                                 |
|----------------------|---------|-----------------------------------------------------------------------------|
| `BUILD_DEPS`         | OFF     | Build internal libmdbx (submodule in `libs/`)                               |
| `BUILD_STATIC_LIB`   | OFF     | Build `mdbx_containers` as a precompiled `.a/.lib` static library           |
| `BUILD_EXAMPLES`     | ON      | Build examples from `examples/`                                             |
| `BUILD_TESTS`        | ON      | Build tests from `tests/`                                                  |


## Core Classes

Each table maps to a unique `MDBX_dbi` within the shared environment.  
Each instance manages its own named DBI and provides standard operations such as `insert_or_assign()`, `find()`, `erase()`, and others.

| Class / Template                 | Description                                               |
|----------------------------------|------------------------------------------------------------|
| `KeyTable<T>`                    | Key-only table (`std::set`-like)                          |
| `KeyValueTable<K, V>`            | Key-value table (`std::map`-like)                         |
| `KeyMultiValueTable<K, V>`       | Key with multiple values (`std::multimap`-like)           |
| `Connection`                     | Manages MDBX environment and logical tables (DBI)         |
| `Transaction`                    | RAII transaction tied to current thread                   |
| `BaseTable`                      | Shared base class for table types                         |

## Supported Data Types

- **Primitives**: `int8_t`, `int32_t`, `uint64_t`, `float`, `double`, etc.  
- **Strings**: `std::string`, `std::vector<char>`, `std::vector<uint8_t>`  
- **STL containers**: `std::vector<T>`, `std::list<T>`, `std::set<T>` — if `T` is serializable  
- **POD types**: serialized using `memcpy` if trivially copyable  
- **Custom types**: must implement `to_bytes()` and `from_bytes()`  

Types can be nested, e.g., `KeyValueTable<std::string, std::vector<MyStruct>>`.

## Transaction Model

- All read/write operations are automatically wrapped in transactions.  
- Manual transactions are available via `Transaction txn(conn, TransactionMode::WRITABLE);`.  
- Transactions are RAII-managed — they commit or roll back on destruction.  
- Nested transactions are **not** supported (MDBX limitation).  
- Transactions are thread-safe — each thread has its own transaction via `TransactionTracker`.

## Internals

- `Connection` internally uses `TransactionTracker` to bind transactions to threads.  
- Serialization is performed via `serialize_value()` / `deserialize_value()` from `detail/serialization.hpp`.  
- Supports both custom `to_bytes()` / `from_bytes()` and fallback to `std::is_trivially_copyable`.  

## Named Tables

Each table (`KeyValueTable`, `KeyTable`, etc.) is associated with a named sub-database (`MDBX_dbi`) within the same MDBX file.  
This allows multiple logical containers to coexist in a single physical database file.

Tables are initialized with a unique `table_name` specified in the `Config`:

```cpp
Config config;
config.db_path = "data.mdbx";
config.table_name = "orders";

KeyValueTable<std::string, int> kv(config);
```

## Error Handling

- MDBX-specific errors are wrapped with `check_mdbx()` and throw `MdbxException` with human-readable messages.  
- Type mismatches or serialization errors (including invalid data sizes) throw `std::runtime_error`.  
- Missing keys return `std::nullopt` from `.find()` (or empty collections where applicable).  

## Code Style: Git Commit Convention

The project follows [Conventional Commits](https://www.conventionalcommits.org/) for git history clarity and automation.

- Use prefixes indicating the type of change:  
  `fix:`, `refactor:`, `example:`, `test:`, `docs:`, `feat:`, etc.
- Optional scope in parentheses:  
  `fix(include):`, `refactor(server):`, `example(codex):`
- Commit message is short and imperative:  
  Examples: `fix(include): remove redundant header`, `refactor(server): simplify transaction logic`

Format:  
```text
type(scope): short message
```