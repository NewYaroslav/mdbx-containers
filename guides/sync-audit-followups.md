# Sync Audit Follow-ups

This note records the follow-up order from the repository-wide sync audit at
`main` commit `60a5b32479f2d69a78ec9f9e404ab717fb59e92c`. Keep the fixes split
into small PRs so behavior, storage format, and build hygiene remain reviewable.

## Immediate PR Sequence

### PR #150: Reserved DBI hardening

- Add one shared `is_reserved_dbi_name()` check for the `_mdbxc_` namespace.
- Reject public user table names that use reserved internal DBI prefixes.
- Reject incoming `ChangeOp` entries targeting reserved DBIs before apply.
- Keep internal stores able to open their reserved DBIs through an internal
  bypass path, not through the public table-name path.
- Add tests for `Put`, `Delete`, and `ClearTable` attempts against reserved
  DBIs, including full rollback of the rejected push page.

### PR #151: External transaction provenance

- Expose enough `Transaction` identity to verify its owning MDBX environment.
- Add a central `BaseTable` helper for external transaction validation.
- Reject `Transaction&` / raw transaction handles that belong to another
  `Connection` before table code uses the table DBI handle.
- Cover cross-connection negative cases for the main table families and
  multi-table helpers such as `VectorStore` / `HashedKeyValueStore`.

### PR #152: SyncWorker stop-before-apply contract

- Re-check stop/cancellation after `PullResponse` is received and after the
  `ApplyStarted` observer callback.
- Re-check immediately before `SyncEngine::handle_push()`.
- Add a deterministic test where the observer calls `request_stop()` from
  `ApplyStarted` and verifies the pulled page is not applied.

### PR #153: Not-implemented public protocol modes

- Make `PullRequest::request_full_snapshot=true` return an explicit
  permanent/protocol error until full snapshot export/import is implemented.
- Reject or otherwise make unavailable `ConflictPolicy::LastWriterWins` until
  timestamp/version-based apply semantics exist.
- Update docs/tests so public API no longer appears to support these modes.

## Next Agreement Point

After PR #150-#153 are reviewed, agree on the next batch before implementation:

- PR #154: standalone public header coverage for the full installed include
  tree, C++11/C++17, sync enabled/disabled, and transport feature macros.
- PR #155+: signed integral key ordering. Choose the storage-format strategy
  explicitly: order-preserving signed encoding with migration/versioning,
  temporary compile-time rejection for ordered signed keys, or documented
  unsigned physical ordering.

## Later Medium-Risk Follow-ups

- Pruning recovery: track earliest retained sequence and report
  snapshot-required when a replica is behind retained history.
- `VectorStore` collection naming: reject invalid names or use a reversible
  collision-free encoding.
- Transport body limits before full buffering in concrete HTTP/WebSocket
  backends.
- Rate-limit bucket eviction / hard caps.
- WebSocket connect/response/request deadlines.
- Installed package fallback for lowercase-only `mdbxConfig.cmake`.
- Clarify `PullRequest::max_bytes` as a soft page budget and add a separate
  max single-batch limit.
- Canonicalize floating-point zero keys and define NaN key semantics.
