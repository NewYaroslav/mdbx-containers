# Critical Defaults

Mandatory rules for AI coding agents working in `mdbx-containers`.

- Check `git status --short` before editing and do not overwrite user changes.
- Prefer `rg` / `rg --files` for repository search.
- Define a verifiable success criterion for every non-trivial task. Replace
  imperative instructions with declarative goals. Example: instead of
  "improve handler", use "reduce handler latency by 20% on the benchmark in
  tests/bench.cpp".
- State assumptions and show interpretation variants for ambiguous requests.
  Do not make silent choices. If the request is unclear, ask for clarification
  before writing code.
- Prefer the minimal code that solves the task. No speculative features,
  single-use abstractions, or handling of impossible scenarios. If the code can
  be noticeably shortened, rewrite it. Test: would a senior call this
  over-engineered? Then simplify.
- When an agent goes off track, rewind to the point before the error and
  reformulate — do not continue correcting in the same session. Context rot
  accumulates and degrades output quality. Use focused `/compact <hint>` instead
  of auto-compact when possible.
- Keep edits scoped to the requested task and the relevant local style.
- Keep `README.md` and `README-RU.md` synchronized; when one changes, update
  the other in the same change unless the user explicitly narrows the scope.
- Preserve C++11 compatibility unless the change is explicitly C++17-only and
  properly guarded.
- Do not use lambda default captures (`[&]` or `[=]`) in C++ code. List every
  captured variable explicitly, and capture `this` explicitly when member access
  is needed.
- Do not introduce `thread_local` STL scratch buffers in serialization paths.
- Follow MDBX transaction ownership: share one `Connection` per MDBX
  environment, keep at most one active transaction per thread, and never pass
  `Transaction`, raw `MDBX_txn*`, or MDBX cursors across threads.
- Treat `configure()`, `connect()`, `disconnect()`, and `Connection`
  destruction as lifecycle-only operations outside concurrent table activity.
- Use `shutdown()`/`shutdown_for()` for coordinated close paths. `disconnect()`
  is a strict lifecycle close and must fail with `MDBX_BUSY` while transaction
  handles are open; do not abort transactions owned by another thread.
- Preserve read-only table semantics: `BaseTable` strips `MDBX_CREATE` and opens
  existing DBIs with `TransactionMode::READ_ONLY` when `Config::read_only` is
  true; wrappers that open additional DBIs outside `BaseTable` must apply the
  same rule, do not create missing directories, and writes should still fail
  through MDBX.
- For code changes, verify with the narrowest relevant tests, and use both C++11
  and C++17 when the change touches shared headers or template behavior.
