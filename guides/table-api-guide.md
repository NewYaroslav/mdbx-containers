# Table API Guide

## Purpose

Use this guide when deciding which persistent table class to use or when adding
methods to an existing table. It is written for coding agents that need a quick
operational model without re-reading every public header first.

For exact signatures, still check the relevant header under
`include/mdbx_containers/`. Generated copies under `build-*/include` are not a
source of truth.

## Quick Selection

| Need | Use | STL analogue | Notes |
| --- | --- | --- | --- |
| One value per key | `KeyValueTable<K, V>` | `std::map` | Default choice for durable key-value storage. |
| One value per string/byte key with hash lookup | `HashedKeyValueStore<K, V, H, Layout>` | `std::map` | Uses a hash bucket index and verifies original key bytes. |
| One true singleton object in a named table | `ValueTable<V>` | persistent variable | Best for metadata, config snapshots, module state, schema/version records, and checkpoints. |
| Unique keys only | `KeyTable<K>` | `std::set` | Stores serialized keys with empty values. |
| Multiple values per key | `KeyMultiValueTable<K, V>` | `std::multimap` | Preserves repeated identical `(key, value)` pairs. |
| Append-only stable-id sequence | `SequenceTable<V>` | `std::vector`-like append, but sparse | Stable uint64_t ids; erase does not shift indices. |
| Different value types by key | `AnyValueTable<K>` | Heterogeneous key-value store | Caller names value type on each access. |

Choose the narrowest table that represents the data:

- Use `KeyTable` for membership, tags, IDs, indexes, and "seen" sets.
- Use `KeyValueTable` for configuration, object snapshots, latest state, and
  any model where a key has one current value.
- Use `ValueTable` for a true singleton object scoped by table name, such as
  storage metadata, app/config snapshots, module state, scheduler state,
  schema/version records, and durable checkpoints.
- Use `HashedKeyValueStore` when keys are strings or byte vectors and a compact
  hash-index lookup path is preferable to ordering by full original key bytes.
- Use `KeyMultiValueTable` for event lists, secondary indexes, histories, and
  any model where repeated values for the same key must remain visible.
- Use `AnyValueTable` only for small heterogeneous keyed settings where values
  under one stable key domain genuinely need different C++ types. It is not a
  replacement for schema design or a typed singleton object.
- Use `SequenceTable` for profiles by id, event logs, histories, tasks/signals
  with stable numeric ids, and any append-only sequence where sparse indices are
  acceptable.

## Common Table Contract

All public table classes follow the same broad shape:

- They live in `namespace mdbxc` and inherit `BaseTable`.
- They are header-only templates under `include/mdbx_containers/`.
- They provide constructors from `std::shared_ptr<Connection>` and `Config`.
- Constructor `name` opens a named MDBX DBI; increase `Config::max_dbs` when
  tests/examples open several named tables in one environment.
- With `Config::read_only = true`, constructors open existing DBIs using a
  read-only transaction. `BaseTable` clears `MDBX_CREATE` automatically, so
  wrapper defaults can stay the same, but the DBI must already exist and write
  methods should fail through MDBX. Wrappers with internal secondary DBIs must
  apply the same rule to those DBIs too.
- Default `HashedKeyValueStore` uses `LargeValues` and consumes two DBIs per
  logical store: records plus `name + "__hash_index"`. In read-only mode both
  DBIs must already exist.
- `HashedStoreLayout::SmallValues` uses one DUPSORT DBI and stores user payload
  inside duplicate values, so respect `Config::max_dupsort_value_size`.
- Public methods accept an optional `MDBX_txn*` where relevant.
- `const Transaction&` overloads delegate to `.handle()`.
- Automatic operations create a transaction, reuse a thread-bound transaction,
  or use the caller-supplied transaction.
- A supplied `MDBX_txn*` or `Transaction` must belong to the current thread.
  Do not pass transaction handles or MDBX cursors across threads.
- Table wrapper instances are not synchronization primitives. For concurrent
  workers, prefer separate wrappers per thread over the same shared
  `Connection`, or protect one shared wrapper with an external mutex.
- `connect()`, `disconnect()`, `configure()`, and `Connection` destruction are
  lifecycle operations outside concurrent table activity.
- For application stop paths, call `Connection::shutdown()` or
  `shutdown_for(timeout)` from a non-transaction owner thread. Use
  `disconnect()` only when all transaction handles/cursors are already gone.
- Raw MDBX work belongs in private `db_*` helpers.
- Raw MDBX return codes go through `check_mdbx()`.
- Serialization uses `SerializeScratch`, `serialize_key()`,
  `serialize_value()`, and `deserialize_value()`.
- Keep C++11 support. Guard `std::optional`, `std::filesystem`, structured
  bindings, and other C++17 features.

## Bulk Semantics

Bulk methods are similar across tables, but they do not all mean the same thing:

| Method | General meaning |
| --- | --- |
| `load(out, txn)` | Adds table content into an existing output container; do not assume it clears `out`. |
| `retrieve_all(txn)` | Returns a fresh container populated from the table. |
| `operator()()` | Convenience wrapper around `retrieve_all()`. |
| `append(source, txn)` | Adds/upserts source data without deleting unrelated table records. |
| `reconcile(source, txn)` | Synchronizes table content to the source using class-specific rules. |
| `operator=(source)` | Convenience wrapper around `reconcile(source)`. |

Important differences:

- `KeyValueTable::append()` upserts source keys and leaves stale keys intact.
- `KeyValueTable::reconcile()` upserts source keys and deletes stale keys.
- `HashedKeyValueStore::append()` and `reconcile()` follow `KeyValueTable`
  one-value-per-key semantics while maintaining the hash index.
- `KeyTable::append()` inserts missing keys and ignores duplicates.
- `KeyTable::reconcile()` currently clears the table and appends source keys.
- `KeyMultiValueTable::append()` inserts every source pair as a stored pair.
- `KeyMultiValueTable::reconcile()` compares pair multiplicity, keeps matching
  records, deletes surplus records, and inserts missing records.

## KeyValueTable

`KeyValueTable<K, V>` is the default map-like table. It stores one serialized
value per serialized key.

Write methods:

- `insert(key, value)` and `insert(pair)` write only if the key is absent.
- `insert_or_assign(key, value)` and `insert_or_assign(pair)` upsert.
- `append(container)` upserts all source pairs.
- `reconcile(container)` upserts source keys and removes keys not present in
  the source.
- `clear()` removes all pairs.

Read/meta methods:

- `at(key)` returns a value or throws `std::out_of_range`.
- `try_get(key, out)` returns a success flag.
- `find(key)` returns `std::optional<ValueT>` in C++17.
- `find(key)` in C++11 returns `std::pair<bool, ValueT>`.
- `find_compat(key)` is the pair-based compatibility form.
- `range(from_key, to_key)` returns key-value pairs inside an inclusive key
  range using a cursor scan.
- `range_values(from_key, to_key)` returns only values for the same range.
- `contains(key)`, `count()`, `empty()`, and `erase(key)` mirror map-like
  metadata/removal operations.
- `operator[]` returns an assignment proxy.

Restrictions and common mistakes:

- Reading a missing key through `operator[]` inserts and persists a
  default-constructed value.
- Vector bulk input can contain duplicate keys; later writes overwrite earlier
  writes for the same key.
- Retrieval into unique-key containers follows the container's own insertion
  rules.
- `range()` and `range_values()` follow MDBX key ordering, not insertion order.

Use it for:

- Current state by ID.
- Name-to-value maps.
- Durable settings.
- Object snapshots where one key has one authoritative value.

Avoid it when:

- Repeated values for a key are meaningful. Use `KeyMultiValueTable`.
- Only membership matters. Use `KeyTable`.

## HashedKeyValueStore

`HashedKeyValueStore<K, V, H, Layout>` is a map-like table for `std::string` and
byte-vector keys. It stores one value per original key and keeps a separate
hash index for lookup by default. Hash collisions do not affect correctness
because reads, updates, and deletes compare the stored original key bytes after
selecting the hash bucket.

Write/read methods mirror `KeyValueTable` for point lookups and full-table
retrieval: `insert()`, `insert_or_assign()`, `append()`, `reconcile()`,
`at()`, `try_get()`, `find()`/`find_compat()`, `contains()`, `erase()`,
`count()`, `empty()`, `clear()`, `load()`, and `retrieve_all()`. It does not
expose original-key `range()` because records are ordered by hash buckets.

Restrictions and common mistakes:

- The default `XXH3Hasher` is non-cryptographic and only accelerates lookup.
- Use `SipHashHasher` or another stable keyed hasher when external users can
  choose many keys.
- Reuse the same hasher and key material for existing data; changing them makes
  existing records unreachable through normal key lookup until rebuilt.
- The default `HashedStoreLayout::LargeValues` keeps payload bytes out of
  DUPSORT duplicate values and supports large serialized values.
- `HashedStoreLayout::SmallValues` stores `original_key + serialized_value` as
  the duplicate value under `hash64`. It is opt-in and intended for small
  values only.
- Increase `Config::max_dbs` by two per default LargeValues store. SmallValues
  consumes one DBI.
- Do not open the same DBI name with different layouts; the physical formats
  are incompatible.

Use it for:

- URL, path, token, string ID, ticker, or byte-key domains where full-key MDBX
  ordering is not the desired lookup shape.
- Key domains with potentially long keys where a hash bucket narrows lookup
  before original-key verification.

## DUPSORT value limits

`Config::max_dupsort_value_size` proactively rejects oversized duplicate values
before `mdbx_put` when set to a positive value. The default is `-1`, so the
proactive check is disabled and MDBX returns its own storage error. Set an
explicit positive limit, such as `16 * 1024`, to enable an application-level
guard. This limit applies to `KeyMultiValueTable` stored values and
`HashedKeyValueStore<..., HashedStoreLayout::SmallValues>`.

## KeyTable

`KeyTable<K>` is the set-like table. It stores unique serialized keys with empty
MDBX values.

Write methods:

- `insert(key)` writes only if the key is absent and returns whether it inserted.
- `append(container)` inserts missing keys and ignores duplicates.
- `reconcile(container)` clears the table and appends source keys.
- `clear()` removes all keys.

Read/meta methods:

- `contains(key)` checks membership.
- `range(from_key, to_key)` returns stored keys inside an inclusive key range
  using a cursor scan.
- `count()` returns stored key count.
- `empty()` checks whether the table has no keys.
- `erase(key)` removes one key.
- `load(container)`, `retrieve_all()`, and `operator()()` load key sets.

Restrictions and common mistakes:

- There is no user value. Do not encode payloads into fake keys unless the data
  is truly a set.
- `reconcile()` is replacement-oriented and not an incremental set diff.
- Key order follows MDBX key ordering, not insertion order.
- `range()` uses the same MDBX key ordering.

Use it for:

- Tags, membership flags, unique IDs, and secondary "exists" indexes.
- Durable deduplication sets.

Avoid it when:

- A key needs a payload. Use `KeyValueTable`.
- A key needs multiple payloads. Use `KeyMultiValueTable`.

## ValueTable

`ValueTable<V>` stores one strongly typed value in a named MDBX table. It is the
right shape for a real singleton object: the table name identifies the object,
and there is no caller-visible key. It uses a fixed internal key and exposes
value-oriented operations instead of asking the caller to remember a sentinel
key.

Write methods:

- `set(value)` upserts the singleton value.
- `insert(value)` writes only when the singleton is absent.
- `update(fn)` reads the value, applies `fn(ValueT&)`, writes it back, and
  returns whether a value existed.
- `erase()` removes the singleton value.
- `clear()` removes all physical rows from the DBI.

Read/meta methods:

- `get()` returns the value or throws `std::out_of_range`.
- `try_get(out)` returns a success flag.
- `find()` returns `std::optional<ValueT>` in C++17 and a pair in C++11.
- `find_compat()` is the pair-based compatibility form.
- `get_or(fallback)`, `has_value()`, `count()`, and `empty()` expose singleton
  state.

Restrictions and common mistakes:

- `count()` is logical and returns `0` or `1`.
- The singleton invariant is guaranteed only when using `ValueTable`; opening
  the same DBI through another table type can intentionally add extra rows.
- Use `clear()` if you need to remove extra physical rows from a DBI that was
  previously misused through another table type.

Use it for:

- `StorageMetadata`, schema version records, migration state, and other
  database metadata.
- `AppConfigSnapshot`, `ModuleState`, `SchedulerState`, and other coherent
  single-object state.
- Durable checkpoints, last-run summaries, cache manifests, and snapshots that
  should have exactly one current value.

Avoid it when:

- Multiple records share one schema, for example profiles by ID or state by
  account. Use `KeyValueTable<K,V>`.
- The data is a set of IDs or flags without payload. Use `KeyTable`.
- Values need different C++ types under user keys and are not one coherent
  object. Use `AnyValueTable`.

## SequenceTable

`SequenceTable<ValueT>` is a persistent appendable table with stable uint64_t
indices. Indices are stable record identifiers: erasing a record does not shift
following indices, and `append()` uses the maximum existing index plus one.

Write methods:

- `append(value)` stores a value at the next available index and returns the
  assigned index.
- `append_many(container)` appends every value from a container and returns a
  vector of assigned indices.
- `insert_or_assign(id, value)` and `set(id, value)` write a value at a
  caller-chosen index, creating holes when the index was not previously used.
- `erase(id)` removes the record at the given index.
- `clear()` removes all records.

Read/meta methods:

- `at(id)` returns the value or throws `std::out_of_range`.
- `try_get(id, out)` returns a success flag.
- `find(id)` returns `std::optional<ValueT>` in C++17.
- `find_compat(id)` is the C++11 pair-based compatibility form.
- `contains(id)`, `count()`, and `empty()` expose table state.
- `first_index()` / `first_index_compat()` return the smallest stored index.
- `last_index()` / `last_index_compat()` return the largest stored index.
- `load(values)`, `retrieve_all()` return values in ascending index order.
- `load_entries(entries)`, `retrieve_entries()` return `(index, value)` pairs in
  ascending index order.
- `range(from, to)` returns `(index, value)` pairs for an inclusive index range.

Behavior:

- `append()` uses `max(existing_id) + 1`. If the table is empty, the first
  appended id is 0.
- `erase()` leaves holes. Following indices are not shifted.
- `set()` / `insert_or_assign()` can create holes at arbitrary indices.
- `retrieve_all()` returns only existing values in index order; holes are not
  represented.
- `retrieve_entries()` and `range()` return real `(id, value)` pairs so holes
  are visible by absence.
- `range(from, to)` is inclusive on both ends. Returns an empty vector when
  `from > to` or when no indices fall in the range.

Restrictions:

- No `insert(pos)`, `push_front`, `pop_front`, `pop_back`, `compact`, or
  `operator[]`.
- `append()` is not atomic across concurrent transactions; use external
  synchronization or a single transaction when concurrent appends must not
  collide.

Use it for:

- Profiles by id, event logs, histories.
- Tasks or signals with stable numeric identifiers.
- Append-only logs where sparse indices are acceptable.

Avoid it when:

- Dense positional access is needed. Use an in-memory `std::vector`.
- Key-value semantics with arbitrary keys are needed. Use `KeyValueTable`.

`KeyMultiValueTable<K, V>` is the multimap-like table. It stores multiple values
for the same key and preserves exact repeated `(key, value)` pairs.

Write methods:

- `insert(key, value)` always creates a separate stored pair.
- `insert(pair)` delegates to `insert(key, value)`.
- `append(container)` inserts every source pair.
- `reconcile(container)` synchronizes by pair multiplicity.
- `erase(key)` removes all values for a key.
- `erase(key, value)` removes all exact matching repeated pairs and returns the
  number removed.
- `clear()` removes all pairs.

Read/meta methods:

- `find(key)` returns `std::vector<ValueT>` in insertion order for that key.
- `range(from_key, to_key)` returns key-value pairs for all keys inside an
  inclusive key range using a cursor scan.
- `range_values(from_key, to_key)` returns only values for the same range.
- `contains(key)` checks whether any value exists for a key.
- `contains(key, value)` checks whether an exact pair exists.
- `count()` counts all stored pairs.
- `count(key)` counts values under one key.
- `count(key, value)` counts exact repeated pair instances.
- `retrieve_all()` defaults to `std::multimap<KeyT, ValueT>`.
- `retrieve_all_vector()` returns every physical pair as a vector element.

Restrictions and common mistakes:

- Identical repeats are intentional and visible in counts and vector retrieval.
- The internal storage adds a sequence prefix to duplicate value bytes. Public
  reads strip it before deserialization.
- Stored duplicate values include the sequence prefix and serialized payload.
  Large values may exceed MDBX_DUPSORT limits or the proactive
  `Config::max_dupsort_value_size` setting.
- `retrieve_all<std::map>()` is legal but loses data according to `std::map`
  unique-key insertion rules. Prefer the default `std::multimap` or
  `retrieve_all_vector()`.
- `reconcile()` avoids unnecessary writes, but it does not reorder existing
  matching records to match source iteration order.
- `range()` and `range_values()` follow MDBX key ordering; values for the same
  key follow stored duplicate order.

Use it for:

- Event streams grouped by entity.
- Inverted indexes and non-unique secondary indexes.
- Histories where repeated identical values are significant.

Avoid it when:

- Only one current value per key matters. Use `KeyValueTable`.
- Exact repeat preservation is not needed and a unique set is enough. Consider
  `KeyTable` or `KeyValueTable` depending on shape.

## AnyValueTable

`AnyValueTable<K>` stores one value per key, but the value type is selected by
the caller for each operation. It is best treated as a small heterogeneous
keyed store for settings and options, not as a replacement for a typed table or
a typed singleton record.

Write methods:

- `set<T>(key, value)` replaces the value for the key.
- `insert<T>(key, value)` writes only when the key is absent.
- `update<T>(key, fn, create_if_missing)` reads as `T`, applies `fn`, and stores
  the modified value.
- `erase(key)` removes the key and its stored value.

Read/meta methods:

- `get<T>(key)` returns the value or throws `std::out_of_range`.
- `find<T>(key)` returns `std::optional<T>` in C++17.
- `find_compat<T>(key)` is the C++11 pair-based lookup.
- `get_or<T>(key, fallback)` returns a fallback when missing.
- `contains(key)` checks key existence.
- `keys()` returns stored keys only.
- `set_type_tag_check(enabled)` toggles opt-in type-tag prefix validation.

Restrictions and common mistakes:

- Each key stores one value. Setting a new type under the same key replaces the
  old value bytes.
- There is no type-erased value enumeration API.
- Type-tag prefix checking is opt-in through `set_type_tag_check(true)` and is
  disabled by default for compatibility with existing raw records.
- Default type tags are implementation-defined. Durable schemas should
  specialize `AnyValueTypeTag<T>`.
- Raw records behave as type mismatches while tag checking is enabled.

Use it for:

- Small heterogeneous settings keyed by stable IDs, such as `"host"` as
  `std::string`, `"port"` as `int`, and `"enabled"` as `bool`.
- Feature flags, plugin/module options, and local preferences where a shared
  key namespace naturally contains different value types.
- Transitional or extensible option bags where adding a new key should not
  require changing a single aggregate type.

Avoid it when:

- The value schema is known and uniform. Use `KeyValueTable<K,V>`.
- The values together form one coherent config/state object. Use
  `ValueTable<AppConfig>` or another explicit aggregate type.
- Runtime type safety is required for an existing raw table. Enable type-tag
  checks only after planning how old raw records should be rewritten or handled.

## Method Stack For Implementers

Most table methods follow this stack:

```text
public method
  -> with_transaction(mode, optional MDBX_txn*)
    -> private db_* helper
      -> SerializeScratch + serialize_key/serialize_value
        -> MDBX C API call
          -> check_mdbx on raw return codes
```

When adding or changing behavior:

- Keep public overloads grouped together: `MDBX_txn*` first, `Transaction`
  overload beside it.
- Reuse existing private `db_*` helpers when possible.
- Handle expected `MDBX_NOTFOUND` and `MDBX_KEYEXIST` explicitly.
- Close cursors on all paths; use a guard/helper if cursor logic becomes more
  complex.
- Do not change serialized bytes without migration notes and compatibility
  tests.
- Keep docs, examples, and focused tests aligned with behavior changes.

## Testing Expectations

When table behavior changes, add focused tests for:

- Automatic transaction path.
- External `Transaction` overload path.
- Empty table behavior.
- Duplicate keys or duplicate pairs when relevant.
- `load()` into existing containers.
- `retrieve_all()` and vector retrieval where data loss is possible through
  container insertion rules.
- C++11 and C++17 build compatibility for public templates.

Documentation-only changes do not require builds, but run `git diff --check` on
changed files.
