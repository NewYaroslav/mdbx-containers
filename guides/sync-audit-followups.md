# Sync Audit Follow-ups

This note records the follow-up order from the repository-wide sync audit at
`main` commit `60a5b32479f2d69a78ec9f9e404ab717fb59e92c`. Keep the fixes split
into small PRs so behavior, storage format, and build hygiene remain reviewable.

## Completed Audit Fixes

- PR #150 hardened reserved `_mdbxc_` DBI handling for public table names and
  incoming sync apply operations.
- PR #97 added standalone public header smoke coverage.
- PR #128 exposed transport backend feature macros.
- PR #131 covered installed transport provider targets.
- PR #152 tightened the `SyncWorker` stop-before-apply boundary.
- PR #154 rejected foreign external transactions before table, sync engine,
  sync store, or accumulator code can use handles from another environment.
- PR #155 made unsupported sync protocol modes explicit:
  `PullRequest::request_full_snapshot=true` is rejected, and
  `ConflictPolicy::LastWriterWins` cannot be selected until the apply semantics
  exist.
- PR #156 renamed the changelog pull internals enough to clarify that empty
  cursors replay retained changelog history rather than exporting a database
  snapshot.
- PR #158 fixed signed `MDBX_INTEGERKEY` range/cursor ordering by storing signed
  integral keys through an order-preserving unsigned rank.
- PR #159 made integral key storage canonical for supported integer-key widths
  and documented the storage-format break for development DBIs.
- PR #160 canonicalized floating-point zero keys and rejected NaN keys.
- PR #161 added fast-math floating-key regression coverage.
- PR #162 added the sync table coverage matrix.

## Current PR Sequence

### PR #163: Audit follow-up ledger refresh

- Keep this file aligned with the PRs that have already landed.
- Preserve the unresolved audit items as small, reviewable future PRs.

### PR #164: ClearTable focused coverage

- Add wrapper-specific `clear()` capture and replication tests for supported
  sync wrappers.
- Keep implementation status and focused test coverage aligned with
  `guides/sync-table-coverage.md`.

### PR #165: Changelog replay API naming cleanup

- Finish removing the misleading public `pull_full_snapshot()` name from the
  sync engine API.
- Keep empty-cursor semantics documented as retained changelog replay, not a
  full database snapshot.

### PR #166: Structured sync-level errors

- Add machine-readable sync response error metadata for permanent protocol
  rejections such as unsupported `request_full_snapshot`.
- Keep transport retry hints transport-owned; sync-level protocol errors should
  be visible without parsing free-form strings.

### PR #167: Pruning recovery

- Detect pull requests whose cursor is behind retained changelog history.
- Return an explicit snapshot-required sync response instead of streaming
  non-contiguous retained batches.

## Later Medium-Risk Follow-ups

- `VectorStore` collection naming: reject invalid names or use a reversible
  collision-free encoding.
- Remote apply invalidation hooks: notify already-open table/view objects after
  sync changes their backing DBIs so cached in-memory state such as
  `VectorStore` RAM indexes can rebuild instead of going stale.
- Transport body limits before full buffering in concrete HTTP/WebSocket
  backends.
- Rate-limit bucket eviction / hard caps.
- WebSocket connect/response/request deadlines.
- Installed package fallback for lowercase-only `mdbxConfig.cmake`.
- Clarify `PullRequest::max_bytes` as a soft page budget and add a separate
  max single-batch limit.
