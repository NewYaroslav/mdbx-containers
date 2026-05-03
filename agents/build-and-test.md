# Build And Test

## Requirements

- CMake 3.18 or newer.
- A C++11 or newer compiler.
- libmdbx, provided through `MDBXC_DEPS_MODE`.
- Windows users should use MinGW-w64 or Clang; MSVC is not supported yet.

## CMake Options

All project options use the `MDBXC_` prefix.

| Option | Default | Description |
| --- | --- | --- |
| `MDBXC_DEPS_MODE` | `AUTO` | MDBX dependency mode: `AUTO`, `SYSTEM`, or `BUNDLED`. |
| `MDBXC_BUILD_EXAMPLES` | `ON` | Build examples from `examples/`. |
| `MDBXC_BUILD_TESTS` | `ON` | Build tests from `tests/` and register them with CTest. |
| `MDBXC_USE_ASAN` | `ON` | Enable AddressSanitizer for tests/examples when supported. |

## Baseline Commands

Configure, build, and test C++17:

```bash
cmake -S . -B build-cpp17 \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=ON \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_STANDARD=17

cmake --build build-cpp17
ctest --test-dir build-cpp17 --output-on-failure
```

Repeat with C++11 for shared header or template changes:

```bash
cmake -S . -B build-cpp11 \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=ON \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_STANDARD=11

cmake --build build-cpp11
ctest --test-dir build-cpp11 --output-on-failure
```

On Windows, the repository includes helper scripts such as
`build-mingw-17-tests.bat`, `build-mingw-11-tests.bat`, and
`build-mingw-17-examples.bat`.

## CI Expectations

- GitHub Actions builds on Windows with MSYS2/MinGW.
- The matrix covers C++11 and C++17.
- CI uses CMake with Ninja and runs `ctest --output-on-failure`.

## Generated Outputs

- Build directories such as `build-*` are generated outputs.
- `docs/html/` and `docs/latex/` are generated Doxygen outputs.
- Do not use generated copies under build directories as source when editing.
