# 10. No C++20 modules (for now)

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

Modules promise faster builds and real encapsulation. The pure domain is the
only candidate: it has no macros and no third-party headers. The gRPC-facing
code is excluded from the start, because generated protobuf headers are not
modularised. The question is whether the toolchain handles a modular domain
*cleanly* today:

- **CMake 3.28** supports `FILE_SET CXX_MODULES` with Ninja, and scans every
  C++23 source for module dependencies by default. We observed it adding
  `-fmodules-ts -fdeps-format=p1689r5` to every GCC compile in the skeleton,
  whether or not modules are used.
- **GCC 14** module support works but is still marked experimental, and
  mixing `import std;` with textual standard headers is not supported in 14.
- **Clang 19 + libstdc++**: `import std;` requires libc++. With libstdc++,
  we would have to keep textual `#include`s of the standard library inside
  module units.
- **clang-tidy 19** and **clangd** have incomplete support for named
  modules. Our quality gates (ADR-0014, CLAUDE.md) rely on clang-tidy over
  every translation unit.
- **Test code, fuzzers and benchmarks** would consume the domain through
  `import`, which spreads module build-order constraints into every target.

## Decision

- No C++20 modules anywhere in the codebase for now. The domain stays a
  classic static library with headers.
- `CMAKE_CXX_SCAN_FOR_MODULES` is set to `OFF` in
  `cmake/CompilerOptions.cmake`, which removes the dependency-scanning step
  CMake 3.28 otherwise runs on every file.

## Consequences

- Faster, simpler builds today, and all tools (clang-tidy, clangd, ccache,
  sanitizers, libFuzzer) work without caveats.
- The domain's encapsulation comes from the architecture rules (ADR-0002),
  not from module boundaries.
- **Revisit when:**
  - the baseline moves to GCC 15+/Clang 20+;
  - `import std;` works with the chosen standard library;
  - clang-tidy supports named modules.

  At that point, a modular `lockstep.domain` is a contained change: one
  library with no macros in its interface.
