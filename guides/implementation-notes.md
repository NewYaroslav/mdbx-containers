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
- mdbx-containers uses the default MDBX transaction ownership model and does
  not enable `MDBX_NOSTICKYTHREADS`.
- Use one shared `Connection` for one MDBX environment/database, and keep at
  most one active transaction per thread.
- Do not pass `Transaction`, raw `MDBX_txn*`, or MDBX cursors across threads.
  Commit, roll back, abort, reset, and destroy transaction guards on the owning
  thread.
- Treat `Connection::configure()`, `connect()`, `disconnect()`, `cleanup()`, and
  destruction as lifecycle-only operations. They must not race with table
  operations, active transactions, or MDBX cursors.
- `Connection::shutdown()` is the coordinated close API. It requests shutdown,
  rejects new transactions, waits until `TransactionTracker` has no open
  transaction handles, then calls `disconnect()`.
- `Connection::shutdown_for(timeout)` has the same shutdown request semantics
  but returns `false` on timeout. The shutdown request remains active, so new
  transactions stay rejected and callers may retry after workers finish.
- `Connection::disconnect()` is strict: close only when no transaction handles
  remain. It must not abort/reset transactions owned by another thread; return
  a clear `MDBX_BUSY` failure instead.
- Do not call `shutdown()`/`shutdown_for()` from a thread that currently owns an
  active or reset transaction handle; that would wait on itself.
- `Connection::m_mdbx_mutex` protects connection lifecycle/config state and the
  manual transaction map. It is not a global MDBX operation lock, and
  `Connection::transaction()` uses it only to gate new transactions against
  shutdown/disconnect races.
- `TransactionTracker::m_mutex` protects only the thread-id-to-`MDBX_txn*`
  registry and its condition variable. It does not make transaction handles
  cross-thread safe.
- Be careful with current-thread transaction lookup when modifying
  `TransactionTracker`, `Connection`, `Transaction`, or `BaseTable`.

Close-path examples:

```cpp
// Clean lifecycle: no open transaction handles or cursors remain.
conn->disconnect();

// Coordinated application stop.
stop_requested.store(true);
conn->shutdown();
worker.join();

// Bounded service stop; shutdown remains requested after false.
if (!conn->shutdown_for(std::chrono::seconds(2))) {
    request_worker_stop();
    worker.join();
    conn->shutdown();
}
```

MDBX references:

- `mdbx_txn_begin()` documents that a transaction and its cursors must only be
  used by a single thread, and that a thread may only have one transaction at a
  time unless `MDBX_NOSTICKYTHREADS` is used:
  https://libmdbx.dqdkfa.ru/group__c__transactions.html
- `mdbx_env_close_ex()` documents that only one thread may close the
  environment and that all transactions, tables, and cursors must already be
  closed before the environment is closed:
  https://libmdbx.dqdkfa.ru/group__c__opening.html

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

Read-only configuration:

- `Config::read_only` opens the environment with `MDBX_RDONLY`.
- `BaseTable` detects read-only connections, clears `MDBX_CREATE` from DBI flags,
  and opens the named DBI inside a `TransactionMode::READ_ONLY` transaction.
- Wrappers that open additional DBIs outside `BaseTable` must repeat the same
  read-only path: clear `MDBX_CREATE`, use `TransactionMode::READ_ONLY`, and
  open only existing DBIs. The default
  `HashedKeyValueStore<..., HashedStoreLayout::LargeValues>` does this for its
  `name + "__hash_index"` DBI.
- The named DBI must already exist. Do not add code that creates DBIs or writes
  records when `Connection::is_read_only()` is true.
- `Connection::db_init()` must not call `create_directories()` for read-only
  opens; missing paths should fail through MDBX instead of mutating the
  filesystem.
- Keep table constructor defaults convenient for callers: wrapper constructors
  may keep `MDBX_CREATE` in their default flags because `BaseTable` strips it for
  read-only connections and wrapper-specific DBI open code must do the same.

For choosing between `KeyValueTable`, `ValueTable`, `KeyTable`,
`KeyMultiValueTable`, and `AnyValueTable`, and for their operation semantics, see
`guides/table-api-guide.md`.

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

## Sync Subsystem

The optional replication layer lives under `include/mdbx_containers/sync/`
and is gated by `MDBXC_SYNC_ENABLED`. The full design record with
endianness policy, store layouts, codec contract, and what is explicitly
deferred to v0.2 lives in
[`include/mdbx_containers/sync/DESIGN.md`](../include/mdbx_containers/sync/DESIGN.md).

Operational rules:

- Read `DESIGN.md` before changing any sync header, the `ChangeBatchCodec`
  layout, or any store's key encoding. These are wire-format and on-disk
  contracts; changes break data on disk and break other nodes.
- Endianness policy (LE for payloads, BE only when a key participates in a
  numeric range scan) is documented in `DESIGN.md`. Do not "fix" the BE
  seq field in `ChangeLogStore::encode_key` back to LE — the change was
  intentional and required for correct range scan order.
- `IdentityIndexValue` is an opaque structured payload keyed by a
  length-prefixed composite. Variable-size fields inside the value
  (`storage_key`, `revision_key`) are themselves length-prefixed for
  decoding. Do **not** add an extra outer MDBX-value prefix — the value is
  single-valued per key, so an outer prefix would solve no collision there.
- Tombstones in `IdentityIndexStore` use the `IDENTITY_TOMBSTONE` flag bit,
  not `erase()`. Older incoming batches may still need to resolve the
  logical key; real removal is explicit.
- The four stores each have an `ensure_open()` guard on every public
  method. Calling a method before `open()` throws `std::logic_error` rather
  than silently writing to DBI 0. Do not weaken this guard.
- `ConflictPolicy::Reject` is the v0.1 default. `LastWriterWins` is
  opt-in and must be deterministic. Prefer `revision_key` when present; if
  no `revision_key` exists, the exact fallback must be defined in
  `SyncEngine` and documented before use. `time_unix_ns` is metadata, not
  a reliable conflict authority. `Custom` is deferred to v0.2.
- `HashedKeyValueStore`, `KeyMultiValueTable`, and `AnyValueTable` are
  not replicated in v0.1; their wire format is not defined. Do not add
  `record_op()` paths for them without first extending `DESIGN.md`.
