# Sync Production Transport Guide

This guide describes the recommended shape for a production transport around
the sync v0.1 API. It is a deployment checklist, not a new wire protocol.

## Boundary

The sync core owns local MDBX state, changelog pagination, batch apply, worker
lifecycle, and progress callbacks. A concrete transport owns everything related
to the network session:

- TLS/WSS or platform TLS termination;
- authentication material such as bearer tokens, cookies, mTLS principals, or
  signed headers;
- token refresh and rotation;
- remote address and per-client allow/deny lists;
- request ids, trace ids, and structured logging;
- retry classification for HTTP status codes or WebSocket close codes;
- graceful shutdown of sockets, acceptors, and connection pools.

Do not add credentials or deployment metadata to `PullRequest`, `PushRequest`,
`PullResponse`, or `PushResponse`. Keep that metadata in the adapter context
and dispatch only decoded sync DTOs to `SyncEngine`.

## Client Shape

A production client usually wraps one `ISyncPeer` implementation:

1. Read the current credential from a caller-owned provider immediately before
   each network call.
2. Attach the credential to the transport context, for example an
   `Authorization` header or WebSocket handshake metadata.
3. Assign a request id / trace id and log the call before sending.
4. Send the encoded sync DTO through `TransportMessageCodec` or a higher-level
   adapter such as `HttpSyncPeer` / `WebSocketSyncPeer`.
5. Map transport failures into a failed sync response or an exception according
   to the local retry policy.
6. Implement `request_cancel()` by interrupting the active socket/future when
   the transport library supports it.

`examples/sync_20_transport_production_wrapper.cpp` shows this shape without a
real socket dependency. The wrapper uses `DirectSyncPeer` underneath, but keeps
token lookup, request id logging, and cancellation outside the sync DTOs.

## Server Shape

A production server binding should perform policy checks before calling
`SyncEngine`:

1. Authenticate the transport session and map it to one `NodeId`.
2. Validate that `PullRequest::requester` or `PushRequest::sender` matches the
   authenticated identity.
3. Validate `db_id` with explicit `SyncDbAccess` rules.
4. Reject oversized bodies before decoding.
5. Decode the message and dispatch to `HttpSyncServer`,
   `WebSocketSyncServer`, or directly to `SyncEngine`.
6. Return transport-local retry metadata, for example `Retry-After` or a
   WebSocket close code, without adding it to the sync DTO.

The built-in HTTP and WebSocket middleware headers provide small reference
policies for bearer identity, DB access checks, fixed-window limits, response
headers, and metrics hooks.

## Observability

Use `ISyncWorkerObserver` for application-facing status:

- `on_sync_worker_stage_changed()` for start/pull/apply/complete/backoff
  timeline events;
- `on_sync_worker_page_applied()` for durable local progress after each page;
- `on_sync_worker_round_completed()` for success/failure summaries;
- `on_sync_worker_backoff()` for retry delay logging.

`SyncWorkerProgressEstimate` is a best-effort catch-up estimate based on the
latest remote tail cursor. It can report known remaining batches for the
current catch-up round. It cannot predict future writes, network time, or
compression cost.

Transport adapters should log request ids and transport status separately from
worker events. A typical production log line contains:

```text
request_id, peer, db_id, operation, transport_status, sync_ok,
pages_pulled, batches_applied, remote_tail_known, batches_remaining
```

## Shutdown

Graceful shutdown is a caller-owned lifecycle:

1. Stop accepting new inbound sessions.
2. Request worker stop with `SyncWorker::request_stop()` or `stop()`.
3. Interrupt the active transport call through `ISyncPeer::request_cancel()`
   when the transport supports it.
4. Join worker and transport threads.
5. Destroy `SyncWorker` only after its worker thread has exited.
6. Disconnect MDBX connections after all sync objects using them are gone.

Cancellation remains best-effort. A blocking peer that ignores its cancellation
token can still delay `stop()`, `join()`, or destruction until the peer returns.

## Retry Policy

Use transport-level retry classification for network statuses and keep
sync-level conflicts separate:

- HTTP 408, 425, 429, 500, 502, 503, and 504 are normally retryable.
- HTTP 400, 401, 403, 404, 405, 413, and 415 are permanent for the current
  request unless credentials or routing change.
- WebSocket 1001, 1005/1006 observations, 1011, 1012, 1013, and 1014 are
  normally retryable.
- WebSocket 1007, 1008, and 1009 are normally permanent for the current
  request.

`SyncWorker` backoff remains the core retry loop for failed sync rounds.
Transport-specific retry headers and close codes should inform that outer
policy, not change the sync DTO wire format.
