# Critical Defaults

Mandatory rules for AI coding agents working in `mdbx-containers`.

- Check `git status --short` before editing and do not overwrite user changes.
- Prefer `rg` / `rg --files` for repository search.
- Keep edits scoped to the requested task and the relevant local style.
- Keep `README.md` and `README-RU.md` synchronized; when one changes, update
  the other in the same change unless the user explicitly narrows the scope.
- Preserve C++11 compatibility unless the change is explicitly C++17-only and
  properly guarded.
- Do not use lambda default captures (`[&]` or `[=]`) in C++ code. List every
  captured variable explicitly, and capture `this` explicitly when member access
  is needed.
- Do not introduce `thread_local` STL scratch buffers in serialization paths.
- For code changes, verify with the narrowest relevant tests, and use both C++11
  and C++17 when the change touches shared headers or template behavior.
