# Sync Transport Production Notes

This guide describes deployment contracts around the optional HTTP and
WebSocket sync transports. It does not replace the transport API reference; it
records the operational decisions an application should make before exposing a
sync endpoint outside a trusted test process.

## Transport Security Boundary

The sync wire format carries sync DTOs only. Transport-local metadata stays in
the adapter layer:

- bearer tokens, cookies, mTLS principals, and remote addresses;
- HTTP response headers such as `WWW-Authenticate` and `Retry-After`;
- WebSocket close codes and handshake metadata;
- request IDs, trace IDs, and log correlation metadata.

Application code should authenticate the transport session before calling the
sync server adapter. Then it should bind the authenticated session identity to
the expected `NodeId` and DB access policy.

## TLS and WSS

The built-in Simple-Web examples use plain HTTP or WS to keep local smoke tests
simple. Production deployments should put one of these in front of the sync
endpoint:

- TLS termination in a reverse proxy or service mesh;
- an HTTPS/WSS-capable framework binding;
- mTLS at the edge when node identity comes from certificates.

When TLS termination happens outside the process, pass only trusted forwarded
identity metadata into the adapter policy layer. Do not treat arbitrary
`X-Forwarded-*` headers from the public network as authenticated facts.

## Token Rotation

Bearer-token policies should support at least two active credentials during a
rotation window:

1. Register the new token for the same `NodeId` and DB access policy.
2. Roll clients to the new token.
3. Remove the old token after the maximum expected client restart or reconnect
   delay.

Long-lived WebSocket sessions should be closed gracefully when their credential
is revoked. The current adapter contract supports this at the binding layer:
the concrete server can reject new handshakes and close existing sessions using
a policy close code such as `1008`.

## Graceful Shutdown

For HTTP and WebSocket servers, shutdown should happen in this order:

1. Stop accepting new transport requests.
2. Request sync workers to stop.
3. Let in-flight `pull()` / `push()` calls return or hit their transport
   timeout.
4. Join worker and listener threads.
5. Destroy `SyncEngine`, tables, and `Connection` objects.

`SyncWorker::request_stop()` requests cancellation through the active
`CancellationToken` and calls `ISyncPeer::request_cancel()` for the observed
in-flight peer call. Cancellation is best-effort: `stop()`, `join()`, and the
destructor may still wait until a transport that ignores or cannot complete
cancellation returns. Concrete transport clients should use finite timeouts or
their own socket-level cancellation mechanism.

## Retry Policy

HTTP and WebSocket adapters expose retry classification helpers, but they do
not own a global retry scheduler. The core worker backoff loop remains the
portable fallback for failed rounds.

For HTTP, transport status and sync DTO status are separate:

- HTTP `2xx` means the transport request succeeded;
- the decoded `PullResponse::ok` or `PushResponse::ok` still reports sync-level
  success or conflict;
- HTTP `408`, `425`, `429`, `500`, `502`, `503`, and `504` are retryable by
  default;
- HTTP policy/framing failures such as `400`, `401`, `403`, `413`, and `415`
  are permanent until the caller changes credentials, DB access, route, or
  payload size.

Use `http_sync_retry_hint()` when an adapter needs one object containing the
retryable flag plus an optional relative `Retry-After` delay. The helper parses
only `Retry-After: <delta-seconds>`; concrete HTTP bindings should handle
absolute HTTP-date values if they need clock-aware behavior.

For WebSocket, close codes `1001`, `1005`, `1006`, `1011`, `1012`, `1013`, and
`1014` are retryable by default. Close codes produced by policy, malformed
payload, and size limits, such as `1007`, `1008`, and `1009`, are permanent
until the request or session changes.

## Structured Logging

Log transport and worker events with stable fields instead of parsing free-form
messages. Useful fields are:

- local node and remote authenticated node;
- `db_id`;
- direction: pull or push;
- request ID or trace ID;
- HTTP status or WebSocket close code;
- pulled, applied, skipped, and rejected batch counts;
- worker stage and catch-up progress when available.

Avoid logging raw keys, values, bearer tokens, or full serialized sync bodies in
normal production logs.

## Offline and Corporate Builds

Provider functions may call `FetchContent` when a backend dependency target is
not already available. `find_package(mdbx_containers CONFIG REQUIRED)` itself
does not fetch transport dependencies; fetching starts only after calling a
`*_transport_provide()` function.

For offline or controlled builds, prefer one of these approaches:

- set `FETCHCONTENT_SOURCE_DIR_<NAME>` cache variables to audited source trees;
- mirror dependencies through Git URL rewriting configured outside this
  package;
- vendor the dependency in the parent project and point the corresponding
  `FETCHCONTENT_SOURCE_DIR_<NAME>` variable at that source tree;
- patch or fork the dependency helper when a build system needs different
  repository URLs.

The current dependency helpers expose cache variables for pinned tags, but not
for repository URLs. Creating compatible dependency targets ahead of time is
not sufficient by itself to skip `FetchContent`, because the helpers still
populate the expected source trees before wiring the final usage targets.

The provider target should remain the only target linked by application code.
This keeps `MDBXC_SYNC_ENABLED`, backend feature macros, include directories,
libraries, and C++ standard requirements in one place.
