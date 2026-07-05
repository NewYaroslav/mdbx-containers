# Coding Style

## Scope

These rules apply to C++11/14/17 sources, headers, examples, tests, and Doxygen
docs in this repository. Prefer the local style in nearby files when it is more
specific.

## Naming

- Class, struct, and enum names use `CamelCase`.
- Methods and free functions use `snake_case`.
- Class fields must use the `m_` prefix, for example `m_connection`.
- Boolean variables should start with `is`, `has`, `use`, or `enable`; class
  fields should use forms such as `m_is_` or `m_has_`.
- Do not use prefixes such as `b_`, `n_`, or `f_`.
- Prefixes `p_` and `str_` are optional when a function has many variables or
  arguments of different types.

Getter methods may omit `get_` when they behave like property accessors, such as
`size()` or `empty()`. Use `get_` when the method performs computation or when
omitting the prefix would be misleading.

## Semantic Values And Literals

- Avoid magic numbers: name non-obvious numeric values, including byte values
  such as `0xD0`, when the domain meaning is not self-evident.
- Avoid magic literals: name non-obvious strings, bytes, characters, regular
  expressions, paths, flags, and test payloads.
- Use intention-revealing names: prefer names that explain the role of a value,
  for example `invalid_utf8_leading_byte` rather than `byte`.
- Make invalid and edge cases explicit: test data for failure paths should state
  which failure condition it represents.
- Prefer named constants for semantic values: if a value has project or domain
  meaning, bind that meaning to a local constant.
- Separate data meaning from representation: `0xD0` is a representation;
  "truncated UTF-8 leading byte" is the meaning.
- Follow the Principle of Least Astonishment: readers should not need to infer
  why a particular byte, flag, or literal was chosen.

## File Names

- If a file contains one primary class, use `CamelCase`, for example
  `Connection.hpp`.
- If a file contains utilities, helpers, or multiple types, use `snake_case`,
  for example `path_utils.hpp`.

## Include Guards

- Use `#pragma once` and a non-reserved include guard for project-owned `.hpp`
  and `.h` headers.
- Guard names use `MDBX_CONTAINERS_HEADER_<PATH>_<FILE>_<EXT>_INCLUDED`.
- Keep `.ipp` implementation fragments unguarded; they are included from
  guarded headers and should not define standalone include guards.
- Build/configuration macros keep their existing domain names and are not
  forced into the guard naming scheme; examples include
  `MDBX_CONTAINERS_HEADER_ONLY` and
  `MDBX_CONTAINERS_SEPARATE_COMPILATION`.
- Do not rewrite third-party vendored headers such as `detail/xxhash.h` just to
  match project guard naming.

## Lambda Captures

- Do not use lambda default captures (`[&]` or `[=]`) in C++ code.
- List every captured variable explicitly so future edits cannot silently pull
  extra state into the lambda.
- When a lambda calls member functions or accesses fields, capture `this`
  explicitly and list the required locals, for example
  `[this, &key, &value](MDBX_txn* txn)`.

## Documentation

- Prefer `///` Doxygen comments.
- Avoid `/** ... */` unless the surrounding file already requires it.
- Use backslash-style Doxygen tags.
- Keep public documentation concise, technical, and in English.
- Lines should generally stay under 100-120 columns.
- Do not start descriptions with "The".
- Public-facing docs should avoid unnecessary internal layout details.

Use this tag order:

```text
\brief
\tparam
\param
\return
\throws
\pre
\post
\invariant
\complexity
\thread_safety
\note / \warning
```

Rules:

- Every function parameter must have a matching `\param` in signature order.
- Every template parameter must have a matching `\tparam`.
- Non-void functions must document the return value with exactly one `\return`.
- Use `\throws` for each documented exception type.
- Add `\pre`, `\post`, and `\invariant` when meaningful.
- Document algorithmic complexity with `\complexity` when it matters.
- State `\thread_safety` as `Thread-safe`, `Not thread-safe`, or
  `Conditionally thread-safe: ...`.
- Use `\note` and `\warning` only when they add real value.

Example:

```cpp
/// \brief Computes 64-bit hash of the input.
/// \tparam T Input type supporting contiguous byte access.
/// \param data Input value.
/// \return 64-bit hash.
/// \complexity O(n) over input size.
/// \thread_safety Not thread-safe.
template<class T>
uint64_t hash(const T& data);
```

If legacy comments conflict with this guide, follow this guide for new or
updated code.
