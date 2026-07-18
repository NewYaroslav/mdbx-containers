# Примеры синхронизации

Эти примеры показывают API sync v0.1 как набор блоков, не привязанных к
конкретному транспорту. Большинство примеров используют `DirectSyncPeer` или
небольшой буфер в памяти, чтобы поток протокола был виден без HTTP, WebSocket
или IPC-кода. Опциональный HTTP-пример связывает HTTP seam с Simple-Web-Server
и standalone Asio, а WebSocket-пример остаётся framework-neutral.

Синхронизация включается явно. Примеры собираются с `MDBXC_SYNC_ENABLED=1`;
приложениям, которые используют sync, тоже нужно компилировать соответствующий
код с этим макросом.

## Сборка и запуск

```bash
cmake -S . -B tmp/build-examples \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=OFF \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_STANDARD=11

cmake --build tmp/build-examples --target sync_01_lifecycle_direct_peer
tmp/build-examples/bin/examples/sync_01_lifecycle_direct_peer
```

Замените имя цели сборки на любой пример из таблицы ниже. На Windows имя
исполняемого файла заканчивается на `.exe`, например:

```powershell
.\tmp\build-examples\bin\examples\sync_01_lifecycle_direct_peer.exe
```

Реальный HTTP binding example включается отдельно, потому что он скачивает
headers standalone Asio и Simple-Web-Server:

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
```

Реальный WebSocket binding example тоже включается отдельно, потому что он
скачивает headers standalone Asio и Simple-WebSocket-Server.
Simple-WebSocket-Server использует OpenSSL Crypto для WebSocket handshake,
поэтому задайте `OPENSSL_ROOT_DIR`, если CMake не найдёт OpenSSL автоматически:

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

## Порядок чтения

| Пример | Что показывает | Уровень |
| --- | --- | --- |
| `sync_01_lifecycle_direct_peer.cpp` | Один явный цикл write -> pull -> push -> read. | Начальный |
| `sync_02_incremental_direct_peer.cpp` | Курсор получателя и инкрементальные pull-запросы. | Начальный |
| `sync_03_multi_table.cpp` | Поддерживаемые типы таблиц и постраничный pull. | Средний |
| `sync_04_primary_to_replicas.cpp` | Один primary и несколько реплик с независимыми курсорами. | Средний |
| `sync_05_three_node_mesh.cpp` | Попарный обмен без пересылки batches, полученных от других origin-узлов. | Продвинутый |
| `sync_06_threaded_transport.cpp` | Владение объектами по потокам и буфер запросов и ответов в памяти. | Продвинутый |
| `sync_07_worker_observer.cpp` | Фоновый `SyncWorker` и уведомления о прогрессе через `ISyncWorkerObserver`. | Продвинутый |
| `sync_08_transport_boundary.cpp` | Псевдотранспорт, который проверяет контракт границы `ISyncPeer` (`CancellationToken` + `request_cancel()`). | Продвинутый |
| `sync_09_transport_codec.cpp` | Версионированный бинарный `TransportMessageCodec` для DTO запросов и ответов. | Продвинутый |
| `sync_10_custom_transport.cpp` | Минимальный кастомный `ISyncPeer` поверх закодированных byte buffers. | Продвинутый |
| `sync_11_http_adapter.cpp` | Фреймворк-независимый HTTP-адаптер поверх `TransportMessageCodec`. | Продвинутый |
| `sync_12_transport_middleware.cpp` | Allow-list, fixed-budget rate limit и metrics middleware вокруг транспортных адаптеров. | Продвинутый |
| `sync_13_http_simple_web_server.cpp` | Опциональный настоящий HTTP binding через Simple-Web-Server и standalone Asio. | Продвинутый |
| `sync_14_websocket_adapter.cpp` | Framework-neutral WebSocket binary-message seam поверх `TransportMessageCodec`. | Продвинутый |
| `sync_15_http_policy_context.cpp` | Bearer-token, remote-address и `Retry-After` HTTP policy context. | Продвинутый |
| `sync_16_worker_http_transport.cpp` | `SyncWorker` поверх `HttpSyncPeer` и HTTP request-context policy. | Продвинутый |
| `sync_17_websocket_simple_web_server.cpp` | Опциональный настоящий WebSocket binding через Simple-WebSocket-Server и standalone Asio. | Продвинутый |

## Общие правила

- `NodeId` идентифицирует конкретный узел репликации. Сгенерируйте его один
  раз, сохраните и используйте повторно после перезапуска.
- `DbId` идентифицирует одну логическую реплицируемую БД. Все узлы,
  реплицирующие эту логическую БД, должны использовать одинаковый `DbId`.
- Перед локальными записями прикрепите `ThreadLocalChangeAccumulator` через
  `attach_sync_capture()`. После завершения локальной фазы записи вызовите
  `detach_sync_capture()`.
- `PullRequest`, `PullResponse`, `PushRequest` и `PushResponse` - структуры
  данных протокола. Именно эти значения нужно сериализовать и передавать через
  транспорт приложения.
- `TransportMessageCodec` - встроенный версионированный бинарный codec для
  этих DTO. Он сериализует данные запросов и ответов, но не локальное состояние
  отмены.
- Не передавайте между потоками или процессами объекты `Connection`,
  `Transaction` и таблиц, а также необёрнутые дескрипторы MDBX и курсоры.
- Получатель применяет страницу через `SyncEngine::handle_push()`. Одна
  страница применяется в одной локальной транзакции; многостраничный pull не
  является одной глобальной транзакцией.
- Курсор применённых изменений получателя сохраняется в MDBX metadata.
  Следующий pull должен использовать этот курсор, чтобы не применять уже
  обработанные batches повторно.
- Применение удалённых изменений обновляет пользовательские таблицы, но не
  записывает удалённый batch в локальный changelog получателя. Обычный узел не
  становится транзитным relay-узлом.
- Sync v0.1 поддерживает `KeyValueTable`, `KeyTable`, `ValueTable`,
  `SequenceTable` и `VectorStore` через его внутренние поддерживаемые таблицы.
  `AnyValueTable`, `KeyMultiValueTable` и `HashedKeyValueStore` не
  реплицируются, пока для них не описан формат передачи.

## Граница транспорта

`DirectSyncPeer` удобен для тестов, примеров и демонстраций внутри одного
процесса. Рабочий код приложения должен рассматривать структуры запросов и
ответов как данные, которые передаются транспортом:

```text
replica формирует PullRequest
-> транспорт передаёт его исходному узлу
-> исходный узел вызывает SyncEngine::handle_pull()
-> транспорт возвращает PullResponse
-> replica помещает batches в PushRequest
-> replica вызывает SyncEngine::handle_push()
```

Многопоточный пример держит каждый `Connection` и `SyncEngine` в том потоке,
который владеет соответствующей БД. Общий буфер передаёт только значения
запросов и ответов протокола. Эту границу стоит сохранить при замене буфера на
реальный транспорт.

`sync_07_worker_observer.cpp` показывает сторону приложения у фоновой реплики:
`SyncWorker` выполняет pull/apply циклы, а `ISyncWorkerObserver` сообщает
основному коду, когда страницы применены или завершён очередной цикл.

`sync_08_transport_boundary.cpp` фиксирует контракт, которому должен следовать
транспортный адаптер, реализующий `ISyncPeer`. Пример показывает, где
наблюдается `CancellationToken` из `PullRequest` и где
`ISyncPeer::request_cancel()` закрывает выполняющийся вызов. Реальные
адаптеры HTTP и WebSocket используют ту же схему, заменяя очереди в памяти
на сокеты.

`sync_09_transport_codec.cpp` показывает следующий слой: `PullRequest`,
`PullResponse`, `PushRequest` и `PushResponse` кодируются в byte buffer через
`TransportMessageCodec` до пересечения границы. Политики адаптера, такие как
авторизация, rate limit, allow/deny lists, маршрутизация и TLS, должны
оборачивать этот обмен bytes, а не попадать внутрь sync DTO.

`sync_10_custom_transport.cpp` показывает минимальную форму кастомного
транспорта: реализовать `ISyncPeer`, закодировать DTO запроса, отправить bytes
через канал приложения, декодировать DTO ответа и реализовать
`request_cancel()` через собственный механизм прерывания канала.

`sync_11_http_adapter.cpp` показывает границу HTTP-адаптера. `HttpSyncPeer`
реализует `ISyncPeer` поверх абстрактного `IHttpSyncClient`, а
`HttpSyncServer` передаёт уже разобранные HTTP method/target/content-type/body
значения в `SyncEngine`. Реальная HTTP-библиотека добавляет сетевой слой вокруг
этой границы.

`sync_14_websocket_adapter.cpp` показывает границу WebSocket-адаптера.
`WebSocketSyncPeer` реализует `ISyncPeer` поверх абстрактного
`IWebSocketSyncChannel`, а `WebSocketSyncServer` передаёт полные бинарные
сообщения в `SyncEngine`. Реальная WebSocket-библиотека добавляет вокруг этой
границы подключение, сборку фрагментов, ping/pong, backpressure и mapping
close/error.

`sync_12_transport_middleware.cpp` показывает adapter-local policy wrappers.
`SyncPeerMiddleware` может проверять декодированные `NodeId` / `DbId` перед
вызовом peer-а, а `HttpSyncClientMiddleware` может применять route-level policy
вокруг HTTP-shaped обмена bytes. В такие обёртки помещаются простые allow-lists,
fixed-budget rate limits и metrics hooks; они не добавляют auth tokens или
счётчики в wire format sync DTO.
Они не заменяют server-framework authentication или per-remote-client rate
limits перед `HttpSyncServer::handle()`. Метрики считают вызовы middleware
hooks, поэтому общий observer на нескольких слоях может посчитать одно
логическое действие по одному разу на каждом слое.

`sync_15_http_policy_context.cpp` показывает request-context слой, который
конкретный HTTP server binding может запускать перед `HttpSyncServer::handle()`.
Он извлекает bearer token из headers, проверяет remote address и возвращает
header `Retry-After`, когда fixed-window limiter отклоняет запрос.

`sync_16_worker_http_transport.cpp` объединяет фоновый worker с HTTP-shaped
adapter. Replica владеет `SyncWorker` и `HttpSyncPeer`; middleware на стороне
primary аутентифицирует bearer token как `NodeId` replica перед тем, как
`HttpSyncServer` передаст запрос в `SyncEngine`.

`sync_17_websocket_simple_web_server.cpp` - socket-backed WebSocket-вариант.
Он связывает `WebSocketSyncPeer` / `WebSocketSyncServer` с
Simple-WebSocket-Server, отправляет binary frames, проверяет bearer token во
время WebSocket handshake и передаёт аутентифицированный `NodeId` replica в
`WebSocketSyncServerMiddleware` перед dispatch.
