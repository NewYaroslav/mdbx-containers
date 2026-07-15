# Sync Examples

These examples show the sync v0.1 API as transport-agnostic building blocks.
They use `DirectSyncPeer` or a small in-memory wire so the protocol flow is
visible without adding HTTP, WebSocket, or IPC code.

Sync is opt-in. The examples are built with `MDBXC_SYNC_ENABLED=1`; applications
must also compile sync users with that macro enabled.

## Reading Order

| Example | What it demonstrates | Level |
| --- | --- | --- |
| `sync_01_lifecycle_direct_peer.cpp` | One explicit write -> pull -> push -> read lifecycle. | Beginner |
| `sync_02_incremental_direct_peer.cpp` | Receiver cursor and incremental pulls. | Beginner |
| `sync_03_multi_table.cpp` | Supported table types and paginated pulls. | Intermediate |
| `sync_04_primary_to_replicas.cpp` | One primary, multiple independent replica cursors. | Intermediate |
| `sync_05_three_node_mesh.cpp` | Pairwise exchange without forwarding remote-origin batches. | Advanced |
| `sync_06_threaded_transport.cpp` | Thread ownership and an in-memory pull/response wire. | Advanced |

## Common Rules

- `NodeId` identifies one physical participant. Generate it once, persist it,
  and reuse it after restart.
- `DbId` identifies one logical replicated database. All copies of the same
  database must use the same `DbId`.
- Attach `ThreadLocalChangeAccumulator` before writing through supported table
  APIs, then detach it after the local write phase.
- `PullRequest`, `PullResponse`, `PushRequest`, and `PushResponse` are detached
  protocol DTOs. They are the data to serialize over a real transport.
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

The threaded example keeps two `Connection` and `SyncEngine` objects in their
own threads and sends only sync DTOs through the wire buffer. That is the part
to preserve when replacing the buffer with a real transport.
