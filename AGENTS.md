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
- [libmdbx](https://github.com/erthink/libmdbx) (fetched automatically; control via `MDBXC_DEPS_MODE`)

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
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_STATIC_LIB=ON \
    -DMDBXC_BUILD_TESTS=ON \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_STANDARD=17

cmake --build build
```

To run tests:

```bash
cd build
ctest --output-on-failure
```

### CMake Options

All options are prefixed with `MDBXC_` to avoid clashes when used as a
subproject.

| Option                   | Default | Description                                                         |
|--------------------------|---------|---------------------------------------------------------------------|
| `MDBXC_DEPS_MODE`        | AUTO    | Dependency mode for libmdbx: `AUTO`, `SYSTEM` or `BUNDLED`          |
| `MDBXC_BUILD_STATIC_LIB` | OFF     | Build `mdbx_containers` as a precompiled `.a/.lib` static library   |
| `MDBXC_BUILD_EXAMPLES`   | ON      | Build examples from `examples/`                                     |
| `MDBXC_BUILD_TESTS`      | ON      | Build tests from `tests/`                                           |
| `MDBXC_USE_ASAN`         | ON      | Enable AddressSanitizer for tests/examples when supported           |


## Testing

All changes must be verified locally and in CI.

- **Local (Linux)**
  - Configure the project with CMake.
  - Build and run tests with `ctest --output-on-failure`.
  - Check both C++11 and C++17 by setting `-DCMAKE_CXX_STANDARD=11` and `17`.

- **Continuous Integration (CI)**
  - GitHub Actions builds and tests on Windows (MSYS2/MinGW).
  - The matrix covers both C++11 and C++17 standards.
  - Builds use CMake with Ninja and tests run via `ctest --output-on-failure`.

Contributors must ensure that code passes in both standards locally and that
CI is green before submitting a pull request.

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

### Thread-local issue and SerializeScratch

Originally, temporary buffers for serialization used `thread_local` STL containers
(e.g., `std::vector<uint8_t>`). On Windows/MinGW this led to **heap corruption**
and random crashes at thread shutdown, because:

- `thread_local` destructors run when the CRT/heap may already be partially finalized
- STL containers may free memory using a different heap arena than the one they were created in
- destructor order across `thread_local` objects is not guaranteed

To fix this, we replaced all `thread_local` STL buffers with a dedicated helper:

```cpp
struct SerializeScratch {
    alignas(8) unsigned char small[16];
    std::vector<uint8_t> bytes;
    // ...
};
```

* `small[16]` provides a stack-like inline buffer for INTEGERKEY and other small values
* `bytes` is used for larger or variable-sized data, owned by the calling scope
* Returned `MDBX_val` is valid only until the next serialization call on the same scratch

This approach removes dependency on `thread_local` destructors, is portable across compilers,
and avoids MinGW-specific runtime crashes.

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

## Naming Conventions

- Prefix `m_` is required for class fields (e.g., `m_event_hub`, `m_task_manager`).
- Prefixes `p_` and `str_` are optional and can be used when a function or method has more than five variables or arguments of different types.
- Boolean variables should start with `is`, `has`, `use`, `enable` or with `m_is_`, `m_has_`, etc. for class fields.
- Do not use prefixes `b_`, `n_`, `f_`.

## Doxygen Comments

- All comments and Doxygen documentation must be in English.
- Use `/// \brief` before functions and classes.
- Do not start descriptions with the word `The`.

## File Names

- If a file contains only one class, use `CamelCase` (e.g., `TradeManager.hpp`).
- If a file contains multiple classes, utilities, or helper structures, use `snake_case` (e.g., `trade_utils.hpp`, `market_event_listener.hpp`).

## Entity Names

- Class, struct, and enum names use `CamelCase`.
- Method names use `snake_case`.

## Method Naming

- Methods should be named in `snake_case`.
- Getter methods may omit the `get_` prefix when they simply return a reference or value or when they provide access to an internal object and behave like a property (e.g., `size()`, `empty()`).
- Use the `get_` prefix when the method performs computations to produce the returned value or when omitting `get_` would be misleading.