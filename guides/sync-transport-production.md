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

`SyncWorker::request_stop()` does not interrupt a blocking transport by itself.
Concrete transport clients should use finite timeouts or their own cancellation
mechanism.

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

- provide dependency targets before calling the provider function;
- set `FETCHCONTENT_SOURCE_DIR_<NAME>` cache variables to audited source trees;
- mirror the dependency repositories and override `GIT_REPOSITORY` variables in
  the corresponding dependency helper;
- vendor the dependency in the parent project and expose compatible CMake
  targets.

The provider target should remain the only target linked by application code.
This keeps `MDBXC_SYNC_ENABLED`, backend feature macros, include directories,
libraries, and C++ standard requirements in one place.
