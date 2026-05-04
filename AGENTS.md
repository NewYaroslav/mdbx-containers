# AGENTS.md

This file is intentionally small. It is an index for AI coding agents working in
`mdbx-containers`; load only the referenced files that are relevant to the task.

## Read First

- [Coding agent workflow](agents/coding-agent-workflow.md) - default workflow for
  all file-editing tasks.
- [Project overview](agents/project-overview.md) - domain model, public API
  surface, supported table types, and data model.
- [Table API guide](agents/table-api-guide.md) - decision guide for choosing
  table classes, methods, bulk semantics, and table-specific constraints.
- [Codebase orientation](agents/codebase-orientation.md) - practical map for
  finding code, reusing patterns, and extending the library safely.
- [Build and test](agents/build-and-test.md) - CMake options, local checks, CI
  expectations, and platform notes.
- [Implementation notes](agents/implementation-notes.md) - transactions,
  serialization, table naming, error handling, and compatibility constraints.
- [Coding style](agents/coding-style.md) - naming, file layout, and Doxygen rules.
- [Commit conventions](agents/commit-conventions.md) - required format when the
  user asks for a commit.

## Critical Defaults

- Check `git status --short` before editing and do not overwrite user changes.
- Prefer `rg` / `rg --files` for repository search.
- Keep edits scoped to the requested task and the relevant local style.
- Preserve C++11 compatibility unless the change is explicitly C++17-only and
  properly guarded.
- Do not introduce `thread_local` STL scratch buffers in serialization paths.
- For code changes, verify with the narrowest relevant tests, and use both C++11
  and C++17 when the change touches shared headers or template behavior.
