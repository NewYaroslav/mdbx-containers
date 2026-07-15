# Примеры Sync

Эти примеры показывают sync v0.1 как transport-agnostic building blocks. Они
используют `DirectSyncPeer` или маленький in-memory wire, чтобы поток протокола
был виден без HTTP, WebSocket или IPC-кода.

Sync включается явно. Примеры собираются с `MDBXC_SYNC_ENABLED=1`; приложениям,
которые используют sync, тоже нужно компилировать соответствующий код с этим
макросом.

## Сборка И Запуск

```bash
cmake -S . -B tmp/build-examples \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=OFF \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_STANDARD=17

cmake --build tmp/build-examples --target sync_01_lifecycle_direct_peer
tmp/build-examples/bin/examples/sync_01_lifecycle_direct_peer
```

Замените имя target на любой пример из таблицы ниже. На Windows у executable
будет суффикс `.exe`.

## Порядок Чтения

| Пример | Что показывает | Уровень |
| --- | --- | --- |
| `sync_01_lifecycle_direct_peer.cpp` | Один явный цикл write -> pull -> push -> read. | Начальный |
| `sync_02_incremental_direct_peer.cpp` | Cursor получателя и incremental pulls. | Начальный |
| `sync_03_multi_table.cpp` | Поддерживаемые типы таблиц и paginated pulls. | Средний |
| `sync_04_primary_to_replicas.cpp` | Один primary и несколько независимых cursor-ов replica. | Средний |
| `sync_05_three_node_mesh.cpp` | Pairwise exchange без forwarding remote-origin batches. | Продвинутый |
| `sync_06_threaded_transport.cpp` | Thread ownership и in-memory pull/response wire. | Продвинутый |

## Общие Правила

- `NodeId` идентифицирует физического участника. Сгенерируйте его один раз,
  сохраните и используйте повторно после restart.
- `DbId` идентифицирует одну логическую реплицируемую БД. Все копии одной БД
  должны использовать один и тот же `DbId`.
- Подключайте `ThreadLocalChangeAccumulator` до записей через поддерживаемые
  table API и отключайте его после локальной фазы записи.
- `PullRequest`, `PullResponse`, `PushRequest` и `PushResponse` являются
  detached DTO протокола. Именно эти данные нужно сериализовать в реальном
  transport.
- Не передавайте `Connection`, `Transaction`, table objects, raw MDBX handles
  или cursors между потоками или процессами.
- Получатель применяет страницу через `SyncEngine::handle_push()`. Одна
  страница применяется в одной локальной transaction; multi-page pull не
  является одной глобальной transaction.
- Applied cursor получателя сохраняется в MDBX metadata. Следующий pull должен
  использовать этот cursor, чтобы не проигрывать уже применённые batches.
- Remote apply обновляет пользовательские таблицы, но не переписывает remote
  batch в локальный changelog получателя. Обычный узел не становится forwarding
  relay.
- Sync v0.1 поддерживает `KeyValueTable`, `KeyTable`, `ValueTable`,
  `SequenceTable` и `VectorStore` через его внутренние поддерживаемые таблицы.
  `AnyValueTable`, `KeyMultiValueTable` и `HashedKeyValueStore` не
  реплицируются, пока для них не описан wire format.

## Граница Transport

`DirectSyncPeer` удобен для tests, examples и in-process demos. Production-код
должен рассматривать request/response structs как transport payload:

```text
replica builds PullRequest
-> transport sends it to the source node
-> source calls SyncEngine::handle_pull()
-> transport returns PullResponse
-> replica wraps batches in PushRequest
-> replica calls SyncEngine::handle_push()
```

Threaded example держит два `Connection` и два `SyncEngine` в своих потоках и
передаёт через wire buffer только sync DTO. Именно эту границу стоит сохранить
при замене буфера на реальный transport.
