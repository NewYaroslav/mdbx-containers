# Project Overview

## What This Project Is

`mdbx-containers` is a lightweight C++11/17 library that bridges familiar STL
container patterns with libmdbx. It gives users durable key-value storage while
keeping the API close to `std::map`, `std::set`, and related containers.

The CMake target is header-only and exported as an `INTERFACE` target. The
project does not build a static archive because the public API is template-heavy
and compiles in the consumer's translation units.

## Main Goals

- Persist container-like data in a crash-safe MDBX environment.
- Hide low-level MDBX boilerplate behind table classes.
- Support multiple logical tables through named MDBX DBI handles.
- Provide RAII transaction management for automatic and manual transactions.
- Keep the library portable across C++11 and C++17 toolchains.

## Public Table Types

| Type | Status | Purpose |
| --- | --- | --- |
| `KeyValueTable<K, V>` | Active | One value per key, similar to `std::map`. |
| `HashedKeyValueStore<K, V, H, Layout>` | Active | One value per string/byte key with a hash lookup index. |
| `AnyValueTable<K>` | Active | Heterogeneous values by caller-specified type; type-tag prefix checks are not fully implemented yet. |
| `KeyTable<K>` | Active | Key-only table with `std::set`-like membership semantics. |
| `KeyMultiValueTable<K, V>` | Active | Multimap-like table with multiple values per key, including repeated identical pairs. |

For method selection, bulk operation semantics, and table-specific constraints,
see [Table API guide](table-api-guide.md).

Core support classes:

- `Config` - MDBX environment configuration. Use `Config::pathname` for the
  database path.
- `Connection` - owns the MDBX environment and transaction registry.
- `Transaction` - RAII wrapper for MDBX transactions.
- `BaseTable` - common DBI opening and transaction helpers.
- `TransactionTracker` - tracks transactions bound to the current thread.

## Supported Data Shapes

- Primitive and trivially copyable values through byte-copy serialization.
- `std::string`, `std::vector<char>`, and `std::vector<uint8_t>`.
- STL containers such as `std::vector<T>`, `std::list<T>`, and `std::set<T>`
  when their element type is serializable.
- Custom types that implement `to_bytes()` and `from_bytes()`.
- Nested values such as `KeyValueTable<std::string, std::vector<MyStruct>>`.

## Basic Usage Pattern

```cpp
mdbxc::Config config;
config.pathname = "example.mdbx";
config.max_dbs = 4;

auto conn = mdbxc::Connection::create(config);
mdbxc::KeyValueTable<int, std::string> table(conn, "orders");

table.insert_or_assign(1, "created");
auto value = table.find(1);
```

Table names are passed to table constructors and become named MDBX sub-databases
inside the same environment. Increase `Config::max_dbs` when a test or example
opens multiple named tables.

`HashedKeyValueStore` defaults to `HashedStoreLayout::LargeValues`, which opens
two named MDBX sub-databases per logical store: the records table named by the
constructor argument and a `__hash_index` companion table. The opt-in
`SmallValues` layout uses one `MDBX_DUPSORT` DBI and stores
`original_key + serialized_value` in duplicate values, so it is only suitable
for small values. The default `XXH3Hasher` is non-cryptographic and only
accelerates lookup; stored original key bytes are always compared for
correctness. Use a stable keyed hasher such as `SipHashHasher` for untrusted
keys.

## Error Model

- MDBX errors are checked with `check_mdbx()` and reported as `MdbxException`.
- Serialization and type validation errors use standard exceptions such as
  `std::runtime_error`.
- `AnyValueTable` should not be documented as providing full runtime type
  safety until its type-tag TODO is implemented.
- In C++17 builds, lookup APIs can return `std::optional`. C++11 fallback APIs
  use explicit success flags such as `std::pair<bool, ValueT>`.
