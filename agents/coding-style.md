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

## File Names

- If a file contains one primary class, use `CamelCase`, for example
  `Connection.hpp`.
- If a file contains utilities, helpers, or multiple types, use `snake_case`,
  for example `path_utils.hpp`.

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
