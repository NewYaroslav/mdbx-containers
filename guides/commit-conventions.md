# Commit Conventions

Operational rule for AI agents that create commits in this repository.

## When To Commit

- Create commits only when the user explicitly asks.
- Check `git status --short` and `git diff` before committing.
- Include only files related to the requested change.

## Format

- Commit messages must be in English and use Conventional Commits.
- Use the form `type(scope): short summary`.
- Use an imperative, concise summary.
- Every commit must include a descriptive body.

Example:

```bash
git commit -m "docs(guides): split project instructions" -m "Move detailed agent guidance into topic-specific files under guides/ and keep AGENTS.md as a compact index."
```

## Allowed Types

- `feat` - new functionality.
- `fix` - bug fix.
- `refactor` - refactoring without behavior changes.
- `perf` - performance improvements.
- `test` - add or modify tests.
- `docs` - documentation changes.
- `build` - build system or dependency updates.
- `ci` - CI/CD configuration changes.
- `chore` - maintenance tasks that do not affect production code behavior.
