# AGENTS.md

## Overview

**mdbx-containers** is a lightweight C++17 header-only library that bridges standard STL containers (e.g., `std::map`, `std::vector`, `std::set`, `std::unordered_map`) with [libmdbx](https://github.com/erthink/libmdbx), providing high-performance, transactional key-value storage with a simple interface.

It is designed for developers who want to:

- Use persistent containers in C++ without dealing directly with the low-level MDBX API  
- Write transactional, concurrent, and crash-safe key-value storage  
- Keep STL containers synchronized with durable storage efficiently  

## Capabilities

**Agent-like tasks this library enables:**

- Storing STL containers into persistent, transactional MDBX databases  
- Reading them back and keeping them synchronized  
- Performing atomic operations using safe RAII transactions  
- Managing multiple named sub-databases (DBIs) inside a single MDBX file  
- Supporting key-only, key-value, and key-to-multiple-value mappings  
- Enabling concurrent access and fine-grained control over performance  

## Example Use Cases

- Persisting `std::map<int, MyStruct>`-like data using `KeyValueTable<int, MyStruct>`  
- Using `KeyValueTable<std::string, std::vector<SimpleStruct>>` to serialize containers of PODs  
- Creating mappings from `std::string` to STL containers (`vector`, `list`, `set`) transparently  
- Efficiently storing PODs and trivially-copyable types via raw `memcpy` serialization  
- Supporting self-serializable types with custom `to_bytes()` / `from_bytes()` logic  
- Using the same `Connection` for multiple logical tables (e.g., `"i8_i8"`, `"str_list_str"`)  
- Running correctness tests and experiments on real datasets with minimal boilerplate  

## Getting Started

Include the headers and ensure libmdbx is available in your build.  
Either install MDBX separately or use the provided submodule.

```bash
git submodule add https://github.com/NewYaroslav/mdbx-containers.git
```

CMake example:

```bash
cmake -DBUILD_DEPS=ON -DBUILD_STATIC_LIB=ON -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=ON .
```

Check the `examples/` directory for full usage samples.

## Library Entry Points

| Class / Template                  | Purpose                                                    |
|----------------------------------|------------------------------------------------------------|
| `KeyTable<T>`                    | Key-only persistent set (analog of `std::set<T>`)          |
| `KeyValueTable<K, V>`            | Key-value persistent map (like `std::map<K, V>`)           |
| `KeyMultiValueTable<K, V>`       | Multi-value map (like `std::multimap<K, V>`)               |
| `Connection`                     | Owns the MDBX environment and named table handles          |
| `Transaction`                    | Manages safe, RAII-based transactions                      |
| `BaseTable`                      | Common base class for all table types                      |

Each table instance manages its own named DBI and supports standard operations like `insert_or_assign()`, `find()`, and `erase()`.

## Data Types Support

As seen in practice, the following types are directly supported:

- **Primitive types**: `int8_t`, `int32_t`, `int64_t`, etc.  
- **Standard strings**: `std::string`  
- **STL containers**: `std::vector<T>`, `std::list<T>`, `std::set<T>` — where `T` is trivially serializable or has defined `to_bytes()` / `from_bytes()`  
- **User-defined POD structs** (via `memcpy`)  
- **Self-serializable custom types** (manual byte encoding)  

The library is flexible enough to allow mixing and nesting, e.g., `KeyValueTable<std::string, std::vector<MyStruct>>`.

## Transaction Model

All operations are transactional:

- Insertions and reads are automatically wrapped in a transaction  
- Underlying transactions can be batched or nested  
- No manual commit required for single operations  

## Named Tables

Each `KeyValueTable` or `KeyTable` is bound to a named sub-database (`DBI`) within the same MDBX file:

```cpp
KeyValueTable<std::string, int> kv(conn, "my_table_name");
```

This allows multiple logical containers to coexist in the same physical file.

## Error Handling

- Type mismatches or invalid serialization sizes throw `std::runtime_error`  
- Missing keys return `std::nullopt` from `.find()`  
- MDBX-specific errors are wrapped with human-readable messages via `check_mdbx()`  

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