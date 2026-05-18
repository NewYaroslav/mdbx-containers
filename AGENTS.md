# AGENTS.md

This file is intentionally small. It is an index for AI coding agents working in
`mdbx-containers`; start with the critical defaults, then load only the
referenced files that are relevant to the task.

## Read First

- [Critical defaults](guides/critical-defaults.md) - mandatory rules for every
  repository task.
- [Coding agent workflow](guides/coding-agent-workflow.md) - default workflow for
  all file-editing tasks.
- [Project overview](guides/project-overview.md) - domain model, public API
  surface, supported table types, and data model.
- [Table API guide](guides/table-api-guide.md) - decision guide for choosing
  table classes, methods, bulk semantics, and table-specific constraints.
- [Codebase orientation](guides/codebase-orientation.md) - practical map for
  finding code, reusing patterns, and extending the library safely.
- [Build and test](guides/build-and-test.md) - CMake options, local checks, CI
  expectations, and platform notes.
- [Implementation notes](guides/implementation-notes.md) - transactions,
  serialization, table naming, error handling, and compatibility constraints.
- [Coding style](guides/coding-style.md) - naming, file layout, and Doxygen rules.
- [Commit conventions](guides/commit-conventions.md) - required format when the
  user asks for a commit.

## Critical Defaults

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
