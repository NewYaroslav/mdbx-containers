# Sync Benchmark

`sync_tick_hub_benchmark` measures the sync core for hub-style tick chunks. It
uses `DirectSyncPeer` in one process, so the reported timings cover local
changelog pull, pagination, decode, and local apply work, not network latency.

## Build

```bash
cmake -S . -B tmp/build-bench \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=OFF \
    -DMDBXC_BUILD_EXAMPLES=OFF \
    -DMDBXC_BUILD_BENCHMARKS=ON \
    -DCMAKE_CXX_STANDARD=17

cmake --build tmp/build-bench --target sync_tick_hub_benchmark
```

## Run Presets

```bash
tmp/build-bench/bin/benchmarks/sync_tick_hub_benchmark --preset quick
tmp/build-bench/bin/benchmarks/sync_tick_hub_benchmark --preset realistic
```

Without arguments the benchmark runs the `quick` preset. `realistic` is intended
for manual measurement before changing pull pagination or changelog indexing.

List presets:

```bash
tmp/build-bench/bin/benchmarks/sync_tick_hub_benchmark --list-presets
```

Run one custom scenario:

```bash
tmp/build-bench/bin/benchmarks/sync_tick_hub_benchmark \
    origins historical_chunks_per_origin new_chunks_per_origin \
    ticks_per_chunk max_batches max_bytes
```

## CSV Notes

Each scenario prints three phases:

- `full_cold_replica` replays the historical changelog into an empty replica.
- `incremental_hot` appends new chunks and pulls with already-open engines.
- `incremental_after_restart` closes, reopens, and reconstructs both engines
  before appending and pulling another incremental range.

`origin_index_entries` should normally match `origins` after historical seeding.
This confirms that `_mdbxc_origins` is populated and the pull path can skip
origins already at the requester's cursor before seeking exact changelog keys.

Use `pull_ms`, `apply_ms`, `pull_pages`, and `batches_per_sec` for comparing
sync-core changes. `seed_ms` and `restart_ms` are reported separately so they do
not hide pull/apply behavior.
