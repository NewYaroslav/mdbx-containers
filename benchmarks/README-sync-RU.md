# Benchmark Sync

`sync_tick_hub_benchmark` измеряет core sync-путь для hub-style tick chunks.
Он использует `DirectSyncPeer` в одном процессе, поэтому времена показывают
локальный changelog pull, pagination, decode и local apply, а не latency сети.

## Сборка

```bash
cmake -S . -B tmp/build-bench \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=OFF \
    -DMDBXC_BUILD_EXAMPLES=OFF \
    -DMDBXC_BUILD_BENCHMARKS=ON \
    -DCMAKE_CXX_STANDARD=17

cmake --build tmp/build-bench --target sync_tick_hub_benchmark
```

## Presets

```bash
tmp/build-bench/bin/benchmarks/sync_tick_hub_benchmark --preset quick
tmp/build-bench/bin/benchmarks/sync_tick_hub_benchmark --preset realistic
```

Без аргументов benchmark запускает preset `quick`. `realistic` предназначен
для ручных измерений перед изменениями pull pagination или changelog indexing.

Список presets:

```bash
tmp/build-bench/bin/benchmarks/sync_tick_hub_benchmark --list-presets
```

Один custom scenario:

```bash
tmp/build-bench/bin/benchmarks/sync_tick_hub_benchmark \
    origins historical_chunks_per_origin new_chunks_per_origin \
    ticks_per_chunk max_batches max_bytes
```

## CSV

Каждый scenario печатает три фазы:

- `full_cold_replica` проигрывает historical changelog в пустую replica.
- `incremental_hot` добавляет новые chunks и делает pull без restart engines.
- `incremental_after_restart` закрывает, заново открывает и реконструирует оба
  engine, затем добавляет и тянет ещё один incremental range.

`origin_index_entries` обычно должен совпадать с `origins` после historical
seeding. Это подтверждает, что `_mdbxc_origins` заполнен и pull path может
пропускать origins, которые уже находятся на cursor получателя, до точного seek
по changelog key.

Для сравнения sync-core изменений используйте `pull_ms`, `apply_ms`,
`pull_pages` и `batches_per_sec`. `seed_ms` и `restart_ms` выводятся отдельно,
чтобы не смешивать подготовку данных с pull/apply поведением.
