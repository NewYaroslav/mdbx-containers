# Sync Subsystem Design (target: v0.1)

> Reading order for any agent considering changes to the sync subsystem.
> This document locks in decisions that have wire- or disk-format consequences
> so future agents do not silently change them. It also distinguishes
> **already implemented** from **planned for v0.1** to keep the contract
> honest as the codebase evolves.

## Scope

Multi-master replication of logical MDBX tables between node-local envs.
Wire is transport-agnostic, codec is versioned, storage uses named DBIs.

## Already implemented (merged into main)

- Public types in `include/mdbx_containers/sync/`:
  `Common`, `ChangeBatch`, `ChangeOp`, `CodecFlags`, `CodecBounds`,
  `Protocol`, `SyncCursor`, `ConflictPolicy`, `ISyncPeer`,
  `IdentityProvider`, `ChangeBatchCodec`, `SyncWorker`.
- Five system stores under `include/mdbx_containers/sync/stores/`:
  `MetaStore`, `ChangeLogStore`, `OriginIndexStore`, `AppliedStore`,
  `IdentityIndexStore`.
- `ChangeBatchCodec` strict versioned wire format (magic, codec version,
  batch version, batch flags, then payload); rejects unknown mandatory
  flags and version mismatches at both encode and decode.

## Planned before v0.1 release (NOT YET implemented)

- Change capture: pre-commit hook in `Transaction::commit` writes a
  `ChangeBatch` to `ChangeLogStore` inside the same write transaction.
- `SyncEngine` (pull/push/apply protocol logic, gap handling).
- `DirectSyncPeer` for in-process tests of `SyncEngine`.
- Full export/import via `seq=0, BATCH_HAS_MORE` chunks for empty replicas.
- Replicated table operation coverage: `KeyValueTable`, `KeyTable`,
  `ValueTable`, `SequenceTable` (single-writer for `append`;
  `insert_or_assign`/`set`/`erase`/`clear` normal).
- `ConflictPolicy::Reject` is the default; `LastWriterWins` is opt-in.

## What v0.1 does NOT cover (deferred to v0.2)

- `HashedKeyValueStore` — internal hash index layout complicates the wire
  format; deferred until an explicit identity-mapping scheme lands.
- `KeyMultiValueTable` — DUPSORT values need per-value length prefixes.
- `AnyValueTable` — heterogeneous values need type-tag propagation on the wire.
- `IdentityProvider` integration in `BaseTable` — declared in v0.1, no
  write path until HashedKeyValueStore.
- Automatic remap of physical `storage_key` from logical `identity_key`.
- HLC for `LastWriterWins` — `time_unix_ns` is metadata only, not a reliable
  conflict authority. The exact tie-break rule (revision_key priority,
  fallback when no revision_key is supplied) must be defined in `SyncEngine`
  and documented before `LastWriterWins` is used in a non-test path.
- `Custom` conflict resolver — schema-level callback; deferred until the
  first real consumer needs it.
- HTTP and WebSocket transports (`Simple-Web-Server`,
  `Simple-WebSocket-Server`) — guarded build flags; first adapter lands
  after `DirectSyncPeer` integration test.
- `zstd` compression — reserved flag, encoder throws, decoder rejects.

## Endianness policy (do not change)

| Layer | Endianness | Reason |
|-------|------------|--------|
| Payload integer fields | little-endian | Native on Windows/Linux/ARM; round-trip is platform-agnostic and cheap. |
| Wire magic, codec version, batch version | ASCII (`MDBXCSYN`) | Independent of byte order. |
| Codec integer fields (`codec_version`, `batch_version`, `batch_flags`, `seq` field, `time_unix_ns`, `ops_count`, op fields) | little-endian | Never sorted on the wire. |
| `ChangeLogStore` key `seq` part | **big-endian** | Range scans (`prune_up_to`, future gap detection) must preserve numeric order under MDBX bytewise compare. LE would sort 256 before 1. |
| `IdentityIndexStore` length prefix `u32 dbi_name_len` | little-endian | Read and written as a single u32, never sorted. |
| All other key bytes (origins, raw `storage_key`, raw `identity_key`, dbi_name bytes) | opaque bytes | Layout defined by the application, not by the codec. |

If you add a new ordered numeric key in the future: big-endian. If you add
any new payload integer: little-endian.

## Stores

### `_mdbxc_meta` (MetaStore)

| Tag | Field | Type | Notes |
|-----|-------|------|-------|
| `0x01` | `db_uuid` | 16 bytes | Stable for the lifetime of this logical database. |
| `0x02` | `node_id` | 16 bytes | Stable for the lifetime of this replication node. |
| `0x03` | `schema_version` | u32 LE | Bumped only when `ChangeBatch` layout or semantics change incompatibly. |
| `0x04` | `local_seq` | u64 LE | Monotonic per-node counter; `increment_local_seq` is the only mutator. |
| `0x05` | `created_at_ms` | u64 LE | Wall-clock metadata, not authority for any conflict. |

### `_mdbxc_changelog` (ChangeLogStore)

| | |
|---|---|
| Key | `origin_node_id (16 raw) ‖ seq (8 BE)` |
| Value | raw `ChangeBatchCodec::encode()` bytes, opaque to this store |
| Insertion | `MDBX_NOOVERWRITE` so accidental `(origin, seq)` reuse throws |
| Retention | explicit `prune_up_to(origin, up_to)` removes records where `seq <= up_to` |

`prune_up_to` opens a cursor, walks from `(origin, 0)` until `mdbx_cmp > (origin, up_to)`,
deletes each hit, then closes. The boundary comparison is on the bytewise
key, which is why `seq` is big-endian in the key.

### `_mdbxc_origins` (OriginIndexStore)

| | |
|---|---|
| Key | `origin_node_id` (16 raw bytes) |
| Value | u64 LE - max known changelog `seq` for that origin |

This index is a discovery accelerator for hub-style pull. `ChangeLogStore::append`
updates it atomically with the changelog row. When an upgraded database has
legacy changelog rows but no `_mdbxc_origins`, the first writable append
backfills the index by scanning existing changelog keys. Read-only pull keeps
a compatibility fallback: if `_mdbxc_origins` is absent or empty, origin
discovery scans `_mdbxc_changelog`.

`ChangeLogStore::origin_index_matches_changelog()` compares the index against
the changelog-derived origin tails. `ChangeLogStore::rebuild_origin_index()`
is the explicit maintenance path for a manually damaged or otherwise partial
`_mdbxc_origins` DBI; ordinary pull does not rebuild metadata in a read-only
transaction.

These maintenance operations scan the changelog. Use them for startup
diagnostics, manual repair, or rare integrity checks; do not place them in the
normal background-sync loop or per-pull hot path.

### `_mdbxc_applied` (AppliedStore)

| | |
|---|---|
| Key | `origin_node_id` (16 raw bytes) |
| Value | u64 LE — last contiguous applied `seq` |

`SyncEngine` invariant: writes to this store are always contiguous. If
`incoming.seq > last + 1`, the receiver keeps the incoming batch pending
until the gap closes. If `incoming.seq <= last`, the batch is a redundant
replay and is skipped.

### `_mdbxc_identity_index` (IdentityIndexStore) — declared in v0.1, write path deferred

| | |
|---|---|
| Key | `u32 dbi_name_len le ‖ dbi_name bytes ‖ identity_key bytes` |
| Value | `IdentityIndexValue { storage_key, origin_node_id, seq, revision_key, flags }` |

Length-prefix on `dbi_name` is mandatory: without it, `("ab","c")` and
`("a","bc")` collide on the same record. The value layout is opaque and
must contain enough metadata to resolve an `(dbi, identity_key)` entry
back to a physical `storage_key` without re-reading the user table.

`IDENTITY_TOMBSTONE` flag bit marks deleted logical records while keeping
the row readable for older incoming batches that still reference the key.
Real removal is explicit via `erase()`.

## Codec — `ChangeBatchCodec`

See `ChangeBatchCodec.hpp` layout comment for the full byte layout. Locked
contract:

- Magic: 8 bytes `MDBXCSYN`.
- Mandatory unknown batch flag bits → decoder throws.
- `BATCH_COMPRESSED_ZSTD` is reserved and rejected at both encode and
  decode paths in v0.1.
- Encoder rejects `ChangeBatch::version != 1`, `op_type > ClearTable`,
  unknown op flag bits, `OP_TOMBSTONE` with non-empty value, and the
  `OP_HAS_*_KEY` flags with empty payloads.
- Decoder rejects trailing bytes when called with `bytes_read == nullptr`
  or via `decode_exact`.

## Sync flow (planned v0.1 behavior, not yet implemented)

Default round shape, single-writer friendly and the base case for
multi-master:

```
origin A writes
    -> Transaction::commit pre-commit hook
        -> ChangeAccumulator.flush (writes ChangeLogStore + OriginIndexStore
           + IdentityIndexStore)
            -> mdbx_txn_commit() — changes land atomically

receiver B
    -> pull / push via ISyncPeer
        -> SyncEngine.handle_push / handle_pull
            -> begin write txn
                if already_applied(origin, seq): commit no-op
                for op in batch.ops: apply raw dbi_op
                mark_applied(origin, seq)
            -> commit
```

Multi-master initial sync of an empty replica:

```
B: empty cursor
    -> pull request, have = empty
A: detects request_full_snapshot
    -> streams ChangeBatches with seq=0 and BATCH_HAS_MORE until done
B: applies each batch as above
    -> onward sync is incremental pull-from-have
```

## Background worker lifecycle

`SyncWorker` is the minimal background driver for `SyncEngine + ISyncPeer`.
It owns its thread but does not own the engine, peer, or connection. The
runtime state shape is:

```
Stopped -> Starting -> Idle -> Pulling -> Applying -> Idle
                              -> Backoff -> Idle
                              -> Stopping -> Stopped
                              -> Failed
```

Worker invariants:

- no local MDBX transaction is held while waiting in `ISyncPeer::pull()`;
- no local MDBX transaction is held during idle or backoff sleeps;
- pulled pages are applied through `SyncEngine::handle_push()`, so each page
  uses one short local write transaction;
- stop requests call `ISyncPeer::request_cancel()` as a best-effort transport
  hook, and a page returned after stop was requested is not applied;
- `stop()`, `join()`, and destruction may wait for an in-flight peer call to
  return when the concrete transport does not support cancellation;
- lifecycle mutations (`start`, `stop`, `join`, `run_once`) are caller-serialized,
  while `request_stop`, `state`, `last_error`, and `wait_until_state` are
  thread-safe;
- the `SyncWorker` object must outlive its background thread and must not be
  destroyed from callbacks running on that worker thread;
- `Transaction`, raw `MDBX_txn*`, and cursors stay on the thread that opened
  them and never cross the worker boundary.

The worker is a lifecycle/concurrency helper, not a transport. HTTP/WebSocket
peers remain separate adapters over `ISyncPeer`. Transport adapters that can
interrupt blocking I/O should implement `request_cancel()` by using their own
timeout, cancellation token, socket shutdown, or equivalent mechanism.

## Why `prune_up_to` uses cursor walk + `MDBX_NEXT`

MDBX has no batch "delete by key range" primitive. The supported pattern
for walking and deleting a range is:

```
cursor open
cursor get(MDBX_SET_RANGE, lo) -> k
while cmp(k, hi) <= 0:
    cursor_del(MDBX_CURRENT)
    cursor get(MDBX_NEXT) -> k
cursor close
```

After `cursor_del`, the cursor stays logically on the deleted position; the
next `MDBX_NEXT` advances to the next live record. The loop terminates
when the current key compares greater than `hi` (or when the cursor runs
out of records). This is the only documented way; alternatives either
don't exist or fail on the first record.

## Why `IdentityIndexValue` does not get an extra outer value prefix

Only the **key** is subject to MDBX bytewise uniqueness — different logical
keys can collapse to the same key bytes if the composite key is not encoded
unambiguously. That is why the identity-index key uses:

    u32 dbi_name_len le ‖ dbi_name bytes ‖ identity_key bytes

The **value** for a given key is single-valued and never participates in MDBX
key comparison. It is still a structured payload, so variable-size fields
inside the value (`storage_key`, `revision_key`) are length-prefixed where
needed for decoding.

Do not add an extra outer MDBX-value prefix around `IdentityIndexValue`;
the MDBX value length is already known from `MDBX_val::iov_len`.

When HLC or similar lands in v0.2, it goes in as opaque bytes inside
`revision_key`.

## Deferred to v0.2 with no open issue yet

- `meta_schema_version()` currently returns 1; bump rule + migration
  procedure not defined.
- `SyncEngine` API surface (`SyncEngine(Connection&)` vs factory,
  pull/push ordering, per-batch txn commit semantics).
- SyncEngine ownership model relative to `Connection` — currently
  `Connection::attach_sync_engine(SyncEngine*)` is the design direction,
  non-owning, lifetime explicitly documented.
- `PeerRegistry` for multi-peer fan-out — single peer per sync invocation
  in v0.1.
