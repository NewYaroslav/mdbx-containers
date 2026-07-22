# Sync v0.1 Readiness Checklist

This checklist records the current sync surface after the transport, worker,
and example hardening pass. It is a release-readiness map, not a stability
promise: sync remains experimental and opt-in through `MDBXC_SYNC_ENABLED=1`.
For the table-by-table support status, capture paths, and deferred wrapper
rules, see [Sync table coverage matrix](sync-table-coverage.md).

## Ready For v0.1 Use

- Supported table capture paths are explicit: `KeyValueTable`, `KeyTable`,
  `ValueTable`, and `SequenceTable` writes are captured when a
  `ThreadLocalChangeAccumulator` is attached to the writing `Connection`;
  `SyncCaptureScope` provides RAII attach/restore for write phases.
- `VectorStore` is covered indirectly through its internal `SequenceTable` and
  `KeyValueTable` members.
- Standalone writes become standalone sync batches; an explicit transaction
  spanning multiple supported tables becomes one local atomic batch.
- Reads, scans, vector search, and other non-mutating APIs are not captured.
- Remote apply uses `SyncEngine::handle_push()` and commits one pulled page per
  local transaction.
- `SyncWorker` owns the background pull loop, pagination, cancellation tokens,
  observer callbacks, retry backoff, and optional `Retry-After` backoff hints.
- `SyncWorker::status()` exposes a thread-safe snapshot for polling UIs,
  health endpoints, and structured logging code that do not subscribe to
  observer callbacks.
- HTTP and WebSocket framework-neutral seams use `TransportMessageCodec`; DTOs
  do not carry bearer tokens, cookies, remote addresses, request ids, or trace
  ids.
- Ready-made optional backends exist for Simple-Web HTTP, Simple-WebSocket, and
  Kurlyk/libcurl HTTP through feature-gated provider targets.
- Installed-package smoke tests cover exported transport provider targets and
  provider argument validation.
- Negative wire/transport tests cover malformed DTOs, oversized payloads,
  response/request mixups, auth/policy rejection, retry classification, and
  selected cancellation paths.

## Operational Contracts To Preserve

- `SyncWorker`, `SyncEngine`, `ISyncPeer`, and table objects are caller-owned;
  they must outlive callbacks and in-flight transport calls that can reference
  them.
- `SyncWorker` lifecycle methods `start()`, `stop()`, `join()`, and
  `run_once()` are caller-serialized. State and diagnostics readers are
  thread-safe.
- Transport cancellation is best-effort. `request_stop()` cancels the active
  token and calls `ISyncPeer::request_cancel()`, but shutdown can still wait
  for a transport that ignores cancellation.
- `SyncTransportRetryHint` is advisory. `available=false` means the peer did
  not classify the failure. `available=true, retryable=false` means the peer
  classified the current transport failure as permanent.
- `SyncWorker` keeps retrying permanent transport hints by default for backward
  compatibility. Set `SyncWorkerPermanentFailurePolicy::StopWorker` when a
  permanent hint should stop the background loop in `Failed`.
- Authentication, DB allow-list decisions, request ids, trace ids, rate-limit
  headers, HTTP status, and WebSocket close codes remain adapter-local
  metadata.
- `PullRequest::requester` and `PushRequest::sender` still need to match the
  authenticated transport identity before a production endpoint dispatches to
  `SyncEngine`.

## Deferred Table Work

These table families intentionally emit no `ChangeOp` in v0.1:

- `AnyValueTable`, until a wire-level type tag and compatibility policy is
  specified.
- `KeyMultiValueTable`, until the deferred unordered multiset design for
  single-writer or causally serialized updates in `sync/DESIGN.md` is
  implemented and covered by capture and round-trip tests.
- `HashedKeyValueStore`, until the relationship between logical key bytes,
  physical storage keys, and hash-index entries is specified.

Do not add `record_op()` calls to these tables until their wire format and
round-trip tests exist. A partial capture path is worse than no capture because
it can make replication appear successful while logical state diverges.

## Suggested Next PRs

- Add small ergonomics helpers around common worker setup if examples continue
  to repeat the same lifecycle boilerplate.
- Prototype `KeyMultiValueTable` capture/apply using the deferred
  single-writer/serialized unordered multiset design, with repeated-pair
  round-trip tests before enabling it.
- Implement the deferred full snapshot protocol before treating
  `SnapshotRequired` as automatically recoverable by sync itself.
- Define explicit conflict/CRDT semantics before claiming general concurrent
  multi-writer convergence for `KeyMultiValueTable`.
- Design `KeyOrderedMultiValueTable<K, V>` separately if distributed histories
  need order convergence across multi-writer nodes.
- Evaluate whether any `KeyMultiValueTable` framing ideas carry over to
  `AnyValueTable` and `HashedKeyValueStore`.

## Validation Baseline

Before declaring a sync-facing change ready, run the narrow affected tests in
both C++11 and C++17. For shared sync headers and transport contracts, include:

```text
header_sync_umbrella_test
header_sync_transport_umbrella_test
test_transport_middleware
test_http_transport
test_websocket_transport
test_sync_worker
test_sync_replication
```

For benchmark or backend-provider changes, also run the dedicated benchmark,
installed-package, and backend smoke jobs or their local equivalents.
