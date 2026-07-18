# Sync Examples

These examples show the sync v0.1 API as transport-agnostic building blocks.
Most use `DirectSyncPeer` or a small in-memory buffer so the protocol flow is
visible without adding HTTP, WebSocket, or IPC code. The optional HTTP example
binds the HTTP seam to Simple-Web-Server and standalone Asio, while the
WebSocket example stays framework-neutral.

Sync is opt-in. The examples are built with `MDBXC_SYNC_ENABLED=1`; applications
must also compile sync users with that macro enabled.

## Build and Run

```bash
cmake -S . -B tmp/build-examples \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=OFF \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_STANDARD=11

cmake --build tmp/build-examples --target sync_01_lifecycle_direct_peer
tmp/build-examples/bin/examples/sync_01_lifecycle_direct_peer
```

Replace the target name with any example from the table below. On Windows the
executable has the `.exe` suffix, for example:

```powershell
.\tmp\build-examples\bin\examples\sync_01_lifecycle_direct_peer.exe
```

The real HTTP binding example is opt-in because it fetches standalone Asio and
Simple-Web-Server headers:

```bash
cmake -S . -B tmp/build-http-example \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=OFF \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DMDBXC_HTTP_SYNC_EXAMPLE=ON \
    -DCMAKE_CXX_STANDARD=11

cmake --build tmp/build-http-example --target sync_13_http_simple_web_server
tmp/build-http-example/bin/examples/sync_13_http_simple_web_server \
    demo 127.0.0.1 18080
tmp/build-http-example/bin/examples/sync_13_http_simple_web_server \
    worker-demo 127.0.0.1 18080
```

The real WebSocket binding example is also opt-in because it fetches standalone
Asio and Simple-WebSocket-Server headers. Simple-WebSocket-Server uses OpenSSL
Crypto for the WebSocket handshake, so set `OPENSSL_ROOT_DIR` if CMake cannot
find OpenSSL automatically:

```bash
cmake -S . -B tmp/build-ws-example \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=OFF \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DMDBXC_WEBSOCKET_SYNC_EXAMPLE=ON \
    -DCMAKE_CXX_STANDARD=11

cmake --build tmp/build-ws-example --target sync_17_websocket_simple_web_server
tmp/build-ws-example/bin/examples/sync_17_websocket_simple_web_server
```

## Reading Order

| Example | What it demonstrates | Level |
| --- | --- | --- |
| `sync_01_lifecycle_direct_peer.cpp` | One explicit write -> pull -> push -> read lifecycle. | Beginner |
| `sync_02_incremental_direct_peer.cpp` | Receiver cursor and incremental pulls. | Beginner |
| `sync_03_multi_table.cpp` | Supported table types and paginated pulls. | Intermediate |
| `sync_04_primary_to_replicas.cpp` | One primary, multiple independent replica cursors. | Intermediate |
| `sync_05_three_node_mesh.cpp` | Pairwise exchange without forwarding remote-origin batches. | Advanced |
| `sync_06_threaded_transport.cpp` | Thread ownership and an in-memory request/response buffer. | Advanced |
| `sync_07_worker_observer.cpp` | Background `SyncWorker` progress notifications through `ISyncWorkerObserver`. | Advanced |
| `sync_08_transport_boundary.cpp` | Pseudo-transport that exercises the `ISyncPeer` boundary contract (cancellation token + `request_cancel()`). | Advanced |
| `sync_09_transport_codec.cpp` | Versioned binary `TransportMessageCodec` for request/response DTOs. | Advanced |
| `sync_10_custom_transport.cpp` | Minimal custom `ISyncPeer` over encoded byte buffers. | Advanced |
| `sync_11_http_adapter.cpp` | Framework-neutral HTTP-shaped adapter over `TransportMessageCodec`. | Advanced |
| `sync_12_transport_middleware.cpp` | Allow-list, fixed-budget, and metrics middleware around transport adapters. | Advanced |
| `sync_13_http_simple_web_server.cpp` | Optional real HTTP binding over Simple-Web-Server and standalone Asio, including a socket-backed worker demo. | Advanced |
| `sync_14_websocket_adapter.cpp` | Framework-neutral WebSocket binary-message seam over `TransportMessageCodec`. | Advanced |
| `sync_15_http_policy_context.cpp` | Bearer-token, remote-address, and `Retry-After` HTTP policy context. | Advanced |
| `sync_16_worker_http_transport.cpp` | `SyncWorker` running through `HttpSyncPeer` and HTTP request-context policy. | Advanced |
| `sync_17_websocket_simple_web_server.cpp` | Optional real WebSocket binding over Simple-WebSocket-Server and standalone Asio. | Advanced |

## Common Rules

- `NodeId` identifies one replication node. Generate it once, persist it, and
  reuse it after restart.
- `DbId` identifies one logical replicated database. All nodes replicating the
  same logical database must use the same `DbId`.
- Before local writes, attach `ThreadLocalChangeAccumulator` with
  `attach_sync_capture()`. After the local write phase, call
  `detach_sync_capture()`.
- `PullRequest`, `PullResponse`, `PushRequest`, and `PushResponse` are
  transport payload structs. Serialize and deliver these values through the
  transport used by your application.
- `TransportMessageCodec` is the built-in versioned binary codec for those
  DTOs. It serializes request/response data, not local cancellation state.
- Do not pass `Connection`, `Transaction`, table objects, raw MDBX handles, or
  cursors across threads or processes.
- A receiver applies a page with `SyncEngine::handle_push()`. One page is
  applied in one local transaction; a multi-page pull is not one global
  transaction.
- The receiver's applied cursor is persisted in its MDBX metadata. The next
  pull should use that cursor to avoid replaying already applied batches.
- Remote apply updates user tables but does not rewrite the remote batch into
  the receiver's local changelog. A simple node is not a forwarding relay.
- Sync v0.1 supports `KeyValueTable`, `KeyTable`, `ValueTable`,
  `SequenceTable`, and `VectorStore` through its internal supported tables.
  `AnyValueTable`, `KeyMultiValueTable`, and `HashedKeyValueStore` are not
  replicated until their wire formats are defined.

## Transport Boundary

`DirectSyncPeer` is useful for tests, examples, and in-process demos. Production
code should treat the request/response structs as transport payloads:

```text
replica builds PullRequest
-> transport sends it to the source node
-> source calls SyncEngine::handle_pull()
-> transport returns PullResponse
-> replica wraps batches in PushRequest
-> replica calls SyncEngine::handle_push()
```

The threaded example keeps each `Connection` and `SyncEngine` on the thread that
owns its database. The shared buffer carries only sync request/response values.
That is the boundary to preserve when replacing the buffer with a real
transport.

`sync_07_worker_observer.cpp` shows the application side of a background
replica: `SyncWorker` performs pull/apply rounds, while `ISyncWorkerObserver`
notifies foreground code when pages are applied or rounds finish.

`sync_08_transport_boundary.cpp` formalises the contract that any transport
adapter over `ISyncPeer` must respect. It implements a tiny in-memory
adapter that owns no MDBX handles and demonstrates where the
`CancellationToken` from `PullRequest` is observed and where
`ISyncPeer::request_cancel()` closes the in-flight call. Real HTTP and
WebSocket adapters will use the same pattern, swapping the in-memory
queues for sockets.

`sync_09_transport_codec.cpp` shows the next layer down: `PullRequest`,
`PullResponse`, `PushRequest`, and `PushResponse` are encoded into byte buffers
with `TransportMessageCodec` before they cross the boundary. Adapter policy
such as authorization, rate limits, allow/deny lists, routing, and TLS belongs
around that byte exchange, not inside the sync DTOs themselves.

`sync_10_custom_transport.cpp` shows the minimal custom transport shape:
implement `ISyncPeer`, encode request DTOs, send bytes through the application
channel, decode response DTOs, and implement `request_cancel()` with the
channel's own interruption primitive.

`sync_11_http_adapter.cpp` shows the HTTP-shaped adapter seam. `HttpSyncPeer`
implements `ISyncPeer` over an abstract `IHttpSyncClient`, and
`HttpSyncServer` dispatches already-parsed HTTP method/target/content-type/body
values to `SyncEngine`. A real HTTP library supplies the socket layer around
that seam.

`sync_14_websocket_adapter.cpp` shows the WebSocket-shaped adapter seam.
`WebSocketSyncPeer` implements `ISyncPeer` over an abstract
`IWebSocketSyncChannel`, and `WebSocketSyncServer` dispatches complete binary
messages to `SyncEngine`. A real WebSocket library supplies connection setup,
fragment reassembly, ping/pong, backpressure, and close/error mapping around
that seam.

`sync_12_transport_middleware.cpp` shows adapter-local policy wrappers.
`SyncPeerMiddleware` can inspect decoded `NodeId` / `DbId` values before a peer
call, while `HttpSyncClientMiddleware` can enforce route-level policy around an
HTTP-shaped byte exchange. These wrappers are where simple allow-lists,
fixed-budget rate limits, and metrics hooks belong; they do not add auth tokens
or counters to the sync DTO wire format.
They do not replace server-framework authentication or per-remote-client rate
limits before `HttpSyncServer::handle()`. Metrics count middleware hook
invocations, so a shared observer installed at several stacked layers can count
one logical action once per layer.

`sync_15_http_policy_context.cpp` shows the request-context layer a concrete
HTTP server binding can run before `HttpSyncServer::handle()`. It extracts a
bearer token from headers, checks the remote address, and returns a `Retry-After`
header when the fixed-window limiter rejects a request.

`sync_16_worker_http_transport.cpp` combines the background worker with the
HTTP-shaped adapter inside one process. The replica owns `SyncWorker` and
`HttpSyncPeer`; the primary-side middleware authenticates the bearer token as
the replica `NodeId` before `HttpSyncServer` dispatches the request.

`sync_13_http_simple_web_server.cpp worker-demo` runs the same worker shape
through real loopback HTTP sockets. It uses bearer identity, a message-size
guard, paginated pulls, and worker observer callbacks over the Simple-Web-Server
binding.

`sync_17_websocket_simple_web_server.cpp` is the socket-backed WebSocket
counterpart. It binds `WebSocketSyncPeer` / `WebSocketSyncServer` to
Simple-WebSocket-Server, sends binary frames, checks the bearer token during
the WebSocket handshake, and passes the authenticated replica `NodeId` to
`WebSocketSyncServerMiddleware` before dispatch.
