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
  `Common`, `ChangeAccumulator`, `ChangeBatch`, `ChangeOp`, `Cancellation`,
  `CodecBounds`, `CodecFlags`, `ConflictPolicy`, `DirectSyncPeer`,
  `IdentityProvider`, `ISyncCaptureSink`, `ISyncPeer`, `Protocol`,
  `SyncCursor`, `SyncEngine`, `SyncWorker`.
- Five system stores under `include/mdbx_containers/sync/stores/`:
  `MetaStore`, `ChangeLogStore`, `OriginIndexStore`, `AppliedStore`,
  `IdentityIndexStore`.
- `ChangeBatchCodec` strict versioned wire format (magic, codec version,
  batch version, batch flags, then payload); rejects unknown mandatory
  flags and version mismatches at both encode and decode.
- `TransportMessageCodec` strict versioned envelope for transport DTOs:
  `PullRequest`, `PullResponse`, `PushRequest`, and `PushResponse`.
  It length-prefixes nested `ChangeBatchCodec` payloads and does not
  serialize operation-local `CancellationToken` state.
- Framework-neutral HTTP-shaped adapter seam: `HttpSyncPeer`,
  `IHttpSyncClient`, `HttpSyncServer`, and `HttpSyncRoutes`. It defines
  route/content-type/body/status mapping over `TransportMessageCodec` but does
  not open sockets or depend on an HTTP framework.
- Framework-neutral WebSocket-shaped adapter seam: `WebSocketSyncPeer`,
  `IWebSocketSyncChannel`, and `WebSocketSyncServer`. It defines a complete
  binary-message request/response contract over `TransportMessageCodec` but
  does not open sockets, own sessions, or depend on a WebSocket framework.
- `sync/transport.hpp` umbrella for framework-neutral transport seams and
  middleware.
- Optional ready-made Simple-Web HTTP/WebSocket bindings under
  `sync/transports/simple_web/` plus socket-backed examples over Simple-Web-Server /
  Simple-WebSocket-Server, standalone Asio, and a process-supervised HTTP
  node-fleet example over tiny-process-library. These integrations are behind
  explicit CMake options, not mandatory runtime dependencies. The backend
  umbrella is `sync/transports/simple_web.hpp`; HTTP-only or WebSocket-only
  targets can include the narrower backend-specific header.
- Transport middleware helpers: `SyncPeerMiddleware`,
  `HttpSyncClientMiddleware`, allow-list policies, fixed-budget rate limiting,
  HTTP request-context bearer/remote-address/fixed-window policies,
  WebSocket session identity policy, bearer token to `NodeId` binding,
  HTTP retry status classification,
  `Retry-After`/`WWW-Authenticate` rejection headers, and a metrics observer.
  These wrap transport adapters and do not change the sync DTO wire format.
- Change capture hooks: `Connection::attach_sync_capture()`,
  `BaseTable::record_op()`, and the transaction pre-commit hook route table
  writes into `ThreadLocalChangeAccumulator`, which appends one local
  `ChangeBatch` per committing write transaction.
- `SyncEngine` pull / push / apply protocol logic, `DirectSyncPeer`
  in-process transport, detailed apply conflict diagnostics, multi-origin
  pagination, origin-index fallback for legacy changelogs, and
  `make_push_request()` for example / transport code.
- Replicated table operation capture for `KeyValueTable`, `KeyTable`,
  `ValueTable`, and `SequenceTable` normal write paths, including bulk
  upserts, reconcile deletes, singleton value writes, and range erases.
  `SequenceTable` `append()` remains a local single-writer operation with
  existing external synchronisation requirements. `VectorStore` is covered
  indirectly because its persistent write path uses `SequenceTable` and
  `KeyValueTable`.
- End-to-end replication tests cover `ValueTable`, `KeyValueTable`,
  `KeyTable`, `SequenceTable` insert/update/delete including empty serialized
  values, and `VectorStore` add/erase/rebuild through its public API.
- `ConflictPolicy::Reject` is the default. `ConflictPolicy::LastWriterWins`
  is declared for future logical-key conflict resolution but is not used by
  the current raw batch apply path.
- `SyncWorker` background pull/apply lifecycle, cooperative cancellation
  tokens, best-effort peer cancellation hook, and focused worker tests.
- Manual hub-style benchmark (`sync_tick_hub_benchmark`) plus opt-in and
  scheduled stress coverage for multi-origin sync paths.

## v0.1 table support matrix

| Type | Sync v0.1 status | Notes |
|------|------------------|-------|
| `KeyValueTable` | Supported | Captures normal put/delete paths, bulk append, reconcile puts/deletes, range erase, and clear-table ops. |
| `KeyTable` | Supported | Captures insert/delete, range erase, reconcile/clear paths that operate on physical keys. |
| `ValueTable` | Supported | Captures singleton put/delete/clear using its fixed physical key. |
| `SequenceTable` | Supported | Captures set/append/delete/clear against stable `uint64_t` record ids. `append()` remains a local single-writer helper; external synchronization is still required for concurrent appenders. |
| `VectorStore` | Supported indirectly | Does not own a separate wire format. Its persistent writes go through `SequenceTable` and `KeyValueTable` member tables. |
| `AnyValueTable` | Not supported in v0.1 | Deferred until heterogeneous value type tags are part of the sync wire format. |
| `KeyMultiValueTable` | Not supported in v0.1 | Deferred until DUPSORT duplicate-value multiplicity and per-value payload framing are specified. |
| `HashedKeyValueStore` | Not supported in v0.1 | Deferred until hash-index and identity-key mapping semantics are specified. |

Do not add `record_op()` paths for unsupported table types without first
updating this design document and adding round-trip replication tests for the
new wire-format semantics.

## Planned before v0.1 release (NOT YET implemented)

- Full export/import via `seq=0, BATCH_HAS_MORE` chunks for empty replicas.

## What v0.1 does NOT cover (deferred to v0.2)

- `HashedKeyValueStore` — internal hash index layout complicates the wire
  format; deferred until an explicit identity-mapping scheme lands.
- `KeyMultiValueTable` — DUPSORT values need per-value length prefixes.
- `AnyValueTable` — heterogeneous values need type-tag propagation on the wire.
- `IdentityProvider` integration in `BaseTable` — declared in v0.1, no
  write path until HashedKeyValueStore.
- Specialized table types not listed in the implemented scope are not treated
  as sync-covered without explicit wire-format design and round-trip tests.
- Automatic remap of physical `storage_key` from logical `identity_key`.
- HLC or other production authority for logical-key `LastWriterWins`.
  `time_unix_ns` is metadata only, not a reliable conflict authority. The
  current apply path does not maintain enough identity-index conflict state to
  use `LastWriterWins` in a non-test path.
- `Custom` conflict resolver — schema-level callback; deferred until the
  first real consumer needs it.
- Production-grade deployment wrappers for concrete socket-bound HTTP and
  WebSocket transports (`Boost.Beast`, libcurl, Simple-Web-Server, or another
  framework) with deployment-specific lifecycle, TLS, reconnect, and
  observability policy. The optional Simple-Web bindings are convenience and
  reference integrations; production services may still wrap or replace them
  for their own operations model.
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
updates it atomically with the changelog row. Pull uses the indexed tail to skip
origins where the requester already has `last_seq`, then still seeks exact
`have_seq + 1` changelog keys for origins with possible new batches. When an
upgraded database has legacy changelog rows but no `_mdbxc_origins`, the first
writable append backfills the index by scanning existing changelog keys.
Read-only pull keeps a compatibility fallback: if `_mdbxc_origins` is absent or
empty, origin discovery scans `_mdbxc_changelog`.

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
`incoming.seq > last + 1`, the current v0.1 apply path reports a
`SequenceGap` conflict and stores no pending queue; the caller must retry after
missing batches arrive. If `incoming.seq <= last`, the batch is a redundant
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

## Codec - `TransportMessageCodec`

Transport DTOs use a separate envelope from individual `ChangeBatch` records.
This lets HTTP, WebSocket, IPC, or message-queue adapters exchange one
request/response object without inventing per-adapter framing for the sync
payload itself.

Locked contract:

- Magic: 8 bytes `MDBXCPRT`.
- Version: u16 little-endian, currently `2`.
- Message type: u8 (`1=PullRequest`, `2=PullResponse`, `3=PushRequest`,
  `4=PushResponse`).
- Message flags: u32 little-endian, currently zero. Unknown non-zero flags are
  rejected.
- Payload integers are little-endian.
- `SyncCursor` is encoded as `u32 count` followed by `(NodeId, u64 seq)`
  entries.
- `ChangeBatch` values inside pull/push messages are encoded as
  `u32 byte_length` plus exact `ChangeBatchCodec` bytes.
- `PullResponse` carries both `remote_have` (responder applied cursor) and
  optional `remote_tail` (responder changelog tail) so receivers can report
  catch-up progress without changing pagination semantics.
- `CancellationToken` fields in request DTOs are local call-control state and
  are never serialized. Decoded request DTOs contain default non-cancellable
  tokens.
- Decoders reject trailing bytes, truncated messages, invalid boolean values,
  wrong message types, unsupported versions, and configured size-limit
  violations.
- `peek_message_type()` validates the shared envelope and returns only the
  message kind. It is for message-oriented adapters such as WebSocket servers
  that dispatch complete binary messages before decoding the type-specific
  payload.
- A null `CodecBounds` argument uses the default `CodecBounds` limits at this
  transport layer; adapters may pass stricter limits for their deployment.

## Sync flow (current v0.1 core behavior)

Default round shape, single-writer friendly and the base case for
multi-master:

```
origin A writes
    -> Transaction::commit pre-commit hook
        -> ChangeAccumulator.flush (writes ChangeLogStore; OriginIndexStore
           is updated by ChangeLogStore::append)
            -> mdbx_txn_commit() — changes land atomically

receiver B
    -> pull / push via ISyncPeer
        -> SyncEngine.handle_push / handle_pull
            -> begin write txn
                if already_applied(origin, seq): skip
                if sequence gap: conflict / rollback
                for op in batch.ops: apply raw dbi_op
                mark_applied(origin, seq)
            -> commit
```

Application integration contract:

- supported table write methods keep the same public API with or without sync;
- callers do not wrap each individual `insert`, `insert_or_assign`, `erase`,
  `reconcile`, or range erase in a sync-specific call;
- `Connection::attach_sync_capture()` installs the capture sink for commits on
  that connection; supported write operations are recorded by table code and
  flushed by the transaction pre-commit hook;
- a standalone write method call that opens its own transaction commits as one
  local change batch;
- an explicit transaction passed through several supported table calls commits
  those writes atomically and flushes one local change batch;
- reads, searches, range scans, and failed/rolled-back transactions emit no
  change batch;
- local commit never contacts a remote node. Pull/push delivery is performed
  later by explicit protocol code or by `SyncWorker` through an `ISyncPeer`.

Cold replica sync currently uses changelog replay:

```
B: empty cursor
    -> pull request, have = empty
A: handle_pull treats it as a full changelog snapshot across known origins
    -> streams persisted ChangeBatches from seq=1 with pagination limits
B: applies each page as above
    -> onward sync is incremental pull-from-have
```

The reserved `seq=0, BATCH_HAS_MORE` full export/import format remains planned
for v0.1 and is not the current cold-replica implementation.

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
- stop requests cancel the active `PullRequest::cancel_token` and call
  `ISyncPeer::request_cancel()` at most once for each observed in-flight
  peer pull call, and a page returned after stop was requested is not applied;
- a stop request recorded before peer-call activation prevents the next pull
  call from starting;
- `stop()`, `join()`, and destruction may wait for an in-flight peer call to
  return when the concrete transport does not support cancellation;
- lifecycle mutations (`start`, `stop`, `join`, `run_once`) are caller-serialized,
  while `request_stop`, `state`, `last_error`, `last_observer_error`, and
  `wait_until_state` are thread-safe;
- optional `ISyncWorkerObserver` callbacks report coarse sync stages
  (`round`, `pull`, `apply`, `backoff`), page application, round completion,
  and backoff entry synchronously on the thread that runs the sync round;
  observer implementations must outlive the worker, return quickly, and avoid
  caller-serialized lifecycle calls from worker-thread callbacks;
- worker stage and round events include a best-effort progress estimate derived
  from the latest `PullResponse::remote_tail` cursor and the receiver cursor;
  it reports known catch-up progress only, not future writes or wall-clock ETA;
- observer exceptions never fail the sync round and are reported through
  `last_observer_error`; they do not overwrite `last_error`, which remains
  reserved for pull/apply, cancellation, and lifecycle failures;
- the `SyncWorker` object must outlive its background thread and must not be
  destroyed from callbacks running on that worker thread;
- `Transaction`, raw `MDBX_txn*`, and cursors stay on the thread that opened
  them and never cross the worker boundary.

The worker is a lifecycle/concurrency helper, not a transport. HTTP/WebSocket
peers remain separate adapters over `ISyncPeer`. Transport adapters that can
interrupt blocking I/O should poll the request `cancel_token` where possible
and implement `request_cancel()` by using their own timeout, socket shutdown,
or equivalent mechanism when polling alone cannot unblock the operation.

Cancellation intentionally stays minimal in the core API: operation-scoped
tokens plus the existing best-effort peer hook. Do not add callback
registration, generation-based token reuse, or allocation-free state reuse
without evidence from a real transport adapter or benchmark. The preferred next
step for HTTP/WebSocket transports is an adapter-local bridge from
`cancel_token` / `request_cancel()` to the transport library's native timeout,
socket shutdown, or cancellation primitive. Revisit the core API only if that
adapter-local approach proves insufficient, or if benchmark data shows
per-operation cancellation-state allocation on the `pull()` path is a measured
hot spot.

## Transport boundary contract

The `ISyncPeer` interface is the single boundary between the sync core and
any external transport (in-process, HTTP, WebSocket, IPC, message queue).
This section locks in the rules a transport adapter must satisfy, the
places where cancellation must be wired through, and the policy decisions
that stay out of the core API. The design is intentionally minimal: locks
in place until a real adapter shows that core cannot support it.

### Boundary rules

- Only `PullRequest`, `PullResponse`, `PushRequest`, and `PushResponse`
  cross the boundary. Binary adapters should use `TransportMessageCodec`
  for those DTOs unless they deliberately define another documented
  content type. `MDBX_txn*`, `MDBX_cursor*`, `Connection`,
  `SyncEngine`, table objects, the `ISyncCaptureSink`, and the system
  stores (MetaStore, ChangeLogStore, AppliedStore, OriginIndexStore,
  IdentityIndexStore) are thread-owned and never enter a transport
  payload.
- `DirectSyncPeer` forwards request DTOs to another `SyncEngine` in the same
  process and is suitable for tests, examples, and in-process demos.
  `HttpSyncPeer` is a framework-neutral HTTP-shaped adapter over an abstract
  `IHttpSyncClient`; it defines the route/body contract but does not own a
  socket. `WebSocketSyncPeer` is a framework-neutral binary-message adapter
  over an abstract `IWebSocketSyncChannel`; it defines the complete-message
  request/response contract but does not own a socket or session. Production
  code that crosses a process boundary must bind these seams to a transport
  implementation that owns its own connection, threading, and lifecycle.
- A transport adapter does not own the caller's `SyncEngine`. The
  receiver-side `SyncEngine` (the one whose state changed because of a
  remote write) must live on the thread that owns the receiver
  `Connection`. Cross-thread writes are not supported.
- A pull page and the matching apply round are owned by `SyncWorker`
  (background) or by the caller of `run_once()` (foreground). The
  transport's `pull()` returns detached `ChangeBatch` values; the worker
  then calls `SyncEngine::handle_push()` which opens its own short
  write transaction.

### Where socket / RPC timeouts live

Timeouts are an adapter-local concern. The core API exposes no
`timeout` field on `PullRequest`/`PushRequest` and no `set_timeout`
on `ISyncPeer` because transports differ about how a deadline is
expressed (HTTP, libcurl, gRPC, Boost.Asio, raw sockets, ...). The
adapter owns its timeout configuration and is responsible for
honoring it. An adapter that cannot bound a blocking call without
the core's help must say so explicitly in its documentation; do not
silently block forever on the worker thread.

The timeout policy that does belong to the core is the worker
backoff loop: repeated pull failures increase the wait between
attempts up to `SyncWorkerOptions::max_backoff`. That is a cooldown
for failed *rounds*, not a per-call deadline.

### Where the cancellation bridge lives

A transport adapter must implement two cancellation channels, both of
which are already in the public API:

1. `PullRequest::cancel_token` and `PushRequest::cancel_token`.
   `SyncWorker` sets a cancellable token before every `pull()` call and
   requests cancellation on it when `request_stop()` runs. The adapter
   receives the token by value and may poll it during any
   interruptible wait. A `CancellationToken` is cheap to copy and
   non-owning; copies share state with their `CancellationSource`.
2. `ISyncPeer::request_cancel()`. `SyncWorker` calls this hook
   at most once for each observed in-flight `pull()`. The default
   implementation is a no-op so token-only peers stay valid. Adapters
   whose underlying call ignores the token (TCP read, blocking RPC,
   legacy client) must override `request_cancel()` to close their
   socket, shutdown their transport, or call the library's native
   cancellation primitive so the in-flight call returns promptly.

The default adapter-local bridge combines both channels: the adapter
sets the same deadline it would use for a normal timeout, polls the
`cancel_token` while waiting on the underlying socket or future, and
also overrides `request_cancel()` to force the socket into a closed or
half-closed state so any blocking call returns. The bridge must be
documented adapter-side; the core does not assume a specific
mechanism.

Cancellation is best-effort. `SyncWorker::stop()`, `join()`, and
destruction may wait for an in-flight peer call to return when the
adapter cannot interrupt that call. This is acknowledged in
`SyncWorker`'s class docstring and is not a contract violation by
the adapter.

### Where reconnect policy lives

Reconnect policy is transport-local. There is no `reconnect_interval`
on `ISyncPeer` and no retry counter on `SyncWorker` because retry
shapes differ across transports (immediate retry after transient
connection reset, exponential backoff across peer failures, jittered
reconnect for peer discovery, ...). An adapter may own its own
`std::thread` that holds the transport connection and surfaces peer
state through a small `ISyncPeer`-like façade; that façade must still
honor the cancellation bridge above.

The one retry shape that lives in core is the worker backoff loop
on repeated pull failures. It is intentionally distinct from
connection-level reconnect and does not attempt to model
connection-state separately from transport-call failures.

### Where auth, rate limits, TLS, and compression live

Auth, rate limits, allow/deny lists, TLS, and compression are adapter-local.
The core has no
`credentials` field on `PullRequest`/`PushRequest` and no TLS
configuration on `ISyncPeer`. A real adapter typically wraps the
transport with the platform's TLS layer (OpenSSL, Schannel, NSURLSession)
or delegates to a server framework that owns the secure channel.
Identity at the sync layer is `NodeId` (16 bytes); the adapter decides
whether TLS terminates before or after the sync payload is parsed.
Authorization and rate limiting should be implemented as wrappers around
request handling: inspect transport metadata and, when needed, the decoded
DTO header fields (`requester`, `sender`, `db_id`) before dispatching to
`SyncEngine`. Do not put bearer tokens, ACL decisions, or rate-limit counters
inside the sync DTO wire format.

`SyncPeerMiddleware`, `HttpSyncClientMiddleware`, and
`HttpSyncServerMiddleware` are the v0.1 framework-free
building blocks for these wrappers. `NodeDbAllowListPolicy` checks decoded
`requester` / `sender` and `db_id` values. `HttpRouteAllowListPolicy` checks
HTTP-shaped targets before a concrete HTTP client sends bytes.
`HttpSyncRequest` carries adapter-local `headers` and `remote_address` fields;
they are not serialized by `TransportMessageCodec`. `HttpBearerTokenPolicy`,
`HttpBearerNodeIdentityPolicy`, `HttpRemoteAddressAllowListPolicy`, and
`FixedWindowHttpRateLimitPolicy` run on that context before dispatch.
`HttpBearerNodeIdentityPolicy` is the v0.1 authenticated-identity contract:
the bearer token maps to one `NodeId`; a pull request is allowed only when
`PullRequest::requester` matches that authenticated node, and a push request is
allowed only when `PushRequest::sender` matches that authenticated node.
Optional per-token DB access rules validate `db_id` before dispatch.
`SyncDbAccess` makes the intent explicit: a token binding may allow any DB,
deny every DB, or allow only listed DB ids. `HttpBearerNodeIdentityPolicy`
keeps token bindings in `allow any DB` mode until
`allow_db_id_for_token()` switches that token to a restricted list. This keeps
the sync DTOs self-describing while preventing a transport principal from
claiming another node id inside the binary payload.
Rejections may carry response headers such as `WWW-Authenticate` or
`Retry-After`; concrete HTTP bindings must write those headers to the real
response. `HttpSyncHeaders::request_id()` and
`HttpSyncHeaders::trace_id()` define optional adapter-local correlation
headers. They are copied from request to response by the framework-neutral HTTP
server/middleware, but they are not serialized inside sync DTOs.
`FixedBudgetSyncTransportPolicy` is a deterministic fixed-budget limiter useful
for tests, examples, and simple adapters; production adapters can replace it
with a time-window or token-bucket policy while keeping the same middleware
shape. `SyncTransportMetricsObserver` records basic call, rejection, failure,
cancel, and batch counters without changing transport behavior.
`TransportMessageSizePolicy` is a pre-decode guard for HTTP bodies and
WebSocket binary messages. It complements `CodecBounds`: adapters can reject
oversized transport frames before decoding, while the codec still validates the
structured payload.

These helpers do not replace server-framework authentication or
per-remote-client rate limiting before `HttpSyncServer::handle()`. They also
count middleware hook invocations: if one observer is installed at both the
peer layer and HTTP client layer, a forwarded `request_cancel()` can be counted
once per layer.
For WebSocket, decoded DTO policy can wrap `WebSocketSyncPeer` through
`SyncPeerMiddleware`. Server-side bindings can pass a
`WebSocketSyncRequestContext` to `WebSocketSyncServerMiddleware` after the
concrete WebSocket framework authenticates the session. The framework-specific
token, cookie, mTLS principal, or remote address stays outside the sync DTO;
`WebSocketAuthenticatedNodeIdentityPolicy` receives only the resulting
authenticated `NodeId`, an explicit `SyncDbAccess` rule, and one complete
binary message. WebSocket request contexts default to `deny every DB`, so a
binding must opt into `SyncDbAccess::any()` or allow concrete DB ids for that
session. The policy then requires `PullRequest::requester` or
`PushRequest::sender` to match that authenticated node before dispatching to
`WebSocketSyncServer`. `WebSocketSyncServerMiddleware` preserves policy close
codes by throwing `WebSocketSyncRejected`; concrete bindings should catch it
and send `close_code()` as the WebSocket close frame status.
Backpressure, reconnects, ping/pong, and pre-DTO rate limits remain
binding-local.

Transport retry classification is adapter-level. HTTP 2xx means transport
success; the decoded sync response still needs its own `ok/error` handling.
HTTP `408`, `425`, `429`, `500`, `502`, `503`, and `504` are retryable
transport statuses by default. Auth, authorization, routing, content-type,
payload-size, and malformed-body errors (`400`, `401`, `403`, `404`, `405`,
`413`, `415`) are permanent for the current request unless a higher-level
adapter refreshes credentials or changes the request. `Retry-After` is advisory
transport metadata; `SyncWorker` backoff remains the core retry loop for failed
sync rounds. For WebSocket, close code `1000` is success; `1001`, local
observations `1005`/`1006`, and server/transient codes `1011`, `1012`, `1013`,
and `1014` are retryable by default. Policy, malformed payload, and oversized
message codes such as `1007`, `1008`, and `1009` are permanent for the current
request.

`ChangeBatchCodec` already rejects `BATCH_COMPRESSED_ZSTD` at both
encode and decode paths. Adding a real `zstd` backend is a codec
change and belongs in a separate design pass; it is not part of the
transport boundary.

### What the core API explicitly does not do

- It does not own a transport connection.
- It does not expose a per-call timeout on the public DTOs.
- It does not expose authentication, credentials, or tokens.
- It does not own a worker thread for the transport itself; only the
  `SyncWorker` pull/apply loop is provided, and it owns no transport
  state.
- It does not promise graceful shutdown of an in-flight peer call;
  `request_cancel()` is best-effort.

### Adapter-local extension pattern

A concrete socket-bound transport adapter ships as a separate pair of headers
(one client, one server) and is gated by its own build option
(`MDBXC_HTTP_SYNC`, `MDBXC_WEBSOCKET_SYNC`, ...). The adapter:

- implements or reuses `ISyncPeer` on the client side;
- wraps `SyncEngine::handle_pull()` / `handle_push()` on the server
  side, directly, through `HttpSyncServer`, or through `WebSocketSyncServer`;
- owns its own threading model (one acceptor thread, thread pool,
  per-connection thread, ...);
- owns its own timeout configuration;
- documents how its `request_cancel()` translates into the
  underlying transport's interrupt primitive.

The framework-neutral `HttpSyncPeer` / `HttpSyncServer` and
`WebSocketSyncPeer` / `WebSocketSyncServer` seams are part of v0.1. Concrete
server/client bindings remain separate optional integrations; the ready-made
Simple-Web bindings live under `sync/transports/simple_web/` and are not
included by the main sync umbrella header.

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
- Public sync API stability after the first external transport adapter.
- `PeerRegistry` for multi-peer fan-out — single peer per sync invocation
  in v0.1.
