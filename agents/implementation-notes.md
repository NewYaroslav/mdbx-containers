# Implementation Notes

## Header-Only Layout

The public include root is `include/`. The umbrella header is
`include/mdbx_containers.hpp`; individual public headers live under
`include/mdbx_containers/`.

The CMake target is `mdbx_containers`. It is always an `INTERFACE` target that
exports include paths, C++11 requirements, and the MDBX dependency to consumers.
The project intentionally does not build a static archive because the public API
is header-only and template-heavy.

## Transactions

- `Connection` owns the MDBX environment and privately inherits
  `TransactionTracker`.
- `Transaction` is an RAII guard around `MDBX_txn`.
- Automatic table operations create transactions internally.
- Manual transactions are available through `Connection::begin()`,
  `Connection::commit()`, and `Connection::rollback()`.
- Nested MDBX transactions are not supported by default project behavior.
- Be careful with current-thread transaction lookup when modifying
  `TransactionTracker`, `Connection`, `Transaction`, or `BaseTable`.

## Serialization

Serialization helpers live in `include/mdbx_containers/detail/utils.hpp`.

Important invariant:

- Do not reintroduce `thread_local` STL containers as serialization scratch
  buffers. Windows/MinGW can crash at thread shutdown because thread-local STL
  destructors may run after the relevant runtime heap state has changed.

Use `SerializeScratch` for temporary serialized data:

```cpp
struct SerializeScratch {
    alignas(8) unsigned char small[16];
    std::vector<uint8_t> bytes;
};
```

The inline `small` buffer is used for small fixed-size values. `bytes` is used
for larger or variable-size serialized data and is owned by the calling scope.
An `MDBX_val` returned from a serialization call is valid only while the
associated scratch storage remains unchanged.

## Named Tables

Each table opens a named MDBX DBI inside the shared environment. The DBI name is
the table constructor argument, for example:

```cpp
mdbxc::KeyValueTable<std::string, int> orders(conn, "orders");
```

Set `Config::max_dbs` high enough for tests and examples that open several
tables in one environment.

For choosing between `KeyValueTable`, `ValueTable`, `KeyTable`,
`KeyMultiValueTable`, and `AnyValueTable`, and for their operation semantics, see
`agents/table-api-guide.md`.

## Configuration

Use `Config::pathname` for the database file or directory. Path resolution is
covered by `tests/path_resolution_test.cpp` and documented in
`docs/configuration.dox`.

Important fields include:

- `read_only`
- `writemap_mode`
- `readahead`
- `no_subdir`
- `sync_durable`
- `max_readers`
- `max_dbs`
- `max_dupsort_value_size`
- `relative_to_exe`

`Config::max_dupsort_value_size` is a proactive guard for MDBX_DUPSORT
duplicate values. Check it before writes that store user payload in duplicate
values, and throw `std::length_error` when the configured positive limit is
exceeded.

## Compatibility

- Keep shared headers compatible with C++11.
- Guard C++17-only facilities with the existing `__cplusplus >= 201703L`
  pattern or provide a C++11 fallback.
- Avoid MSVC-specific assumptions; Windows CI targets MinGW.
