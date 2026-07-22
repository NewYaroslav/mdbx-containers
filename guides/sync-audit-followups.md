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
- PR #163 refreshed this follow-up ledger after the first audit sequence.
- PR #164 added wrapper-specific `clear()` capture and replication coverage.
- PR #165 removed the misleading public `pull_full_snapshot()` API name and
  documented retained changelog replay semantics.
- PR #166 added machine-readable sync response error metadata for permanent
  protocol rejections.
- PR #167 detects cursors older than retained changelog history and reports
  `SnapshotRequired`.
- PR #168 rejects invalid `VectorStore` collection names before building
  internal DBI names.
- PR #169 exposed `Connection::sync_apply_generation()` and made already-open
  `VectorStore` instances lazily rebuild after remote apply.
- PR #170 added concrete binding body-limit knobs for Simple-Web HTTP,
  Simple-WebSocket, and Kurlyk HTTP.
- PR #171 serialized remote apply commits with cache-backed `VectorStore`
  operations through a connection apply/read barrier.
- PR #172 capped and evicted HTTP fixed-window rate-limit buckets.
- PR #173 added whole-exchange deadlines for the Simple-WebSocket sync client.
- PR #174 supports installed packages that provide lowercase `mdbxConfig`.
- PR #175 split pull page byte budgets from per-batch byte budgets.
- PR #180 added connection-level remote apply observer hooks for cache
  invalidation, metrics, and logging after successful remote apply commits.

## Current PR Sequence

### PR #176: Transport include graph cleanup

- Keep concrete backend headers from depending on the aggregate
  `sync/transport.hpp` header that names the framework-neutral transport
  surface.
- Preserve the public aggregate header for user convenience.

### PR #177: Audit follow-up ledger refresh

- Keep this file aligned with the PRs that have already landed.
- Preserve the unresolved audit items as small, reviewable future PRs.

### PR #178: Full snapshot protocol design

- Specify the future `PullRequest::request_full_snapshot=true` protocol before
  implementing it.
- Keep the current v0.1 behavior explicit: unsupported full snapshots and
  pruned cursors return machine-readable sync errors.

### PR #179: `KeyMultiValueTable` sync design

- Define the unordered multiset framing and apply semantics for duplicate
  values before adding capture support.
- Keep order-sensitive histories deferred to a separate
  `KeyOrderedMultiValueTable<K, V>` design.

## Later Medium-Risk Follow-ups

- Per-DBI remote apply invalidation: `ISyncApplyObserver` currently reports a
  coarse committed apply event. If more cached wrappers appear, extend the hook
  with affected DBI names or table identity filters.
- Framework-level pre-buffer limits: where the concrete HTTP/WebSocket library
  supports it, configure request, response, and frame caps before the complete
  payload is retained in memory. PR #170 added binding-side limits after the
  framework has surfaced the payload.
- Worker/node ergonomics helpers: reduce repeated setup code around
  `SyncCaptureScope`, transport peer construction, worker lifecycle, and common
  production policies.
