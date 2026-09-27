# 10. No C++20 modules (for now)

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

Modules promise faster builds and real encapsulation.

- **Excluded from the start:** the gRPC-facing code. Generated protobuf
  headers are not modularised.
- **The only candidate:** the pure domain. It has no macros, and no
  third-party headers.

The question is whether the toolchain handles a modular domain *cleanly*
today, and whether the benefit is worth the cost.

We probed rather than assumed. A small project exported a `lockstep.probe`
module (global module fragment with `<cstdint>`/`<vector>`) through CMake
`FILE_SET CXX_MODULES`, consumed by an executable. It was built with our
exact toolchain: CMake 3.28, Ninja 1.11, GCC 14.2, Clang 19.1, clang-tidy 19,
ccache 4.9.

| Check | GCC 14 | Clang 19 |
|---|---|---|
| Named module via CMake `FILE_SET CXX_MODULES` | ✅ builds, runs | ✅ builds, runs |
| `import std;` | ❌ no `std` module in libstdc++ 14 | ❌ `module 'std' not found` (needs libc++) |
| clang-tidy on interface unit and consumer | – | ✅ |
| ASan + UBSan build | – | ✅ |
| **ccache** (2 clean builds) | ❌ every module-related compile uncacheable ("unsupported compiler option": `-fmodules-ts`) | ⚠️ interface unit uncacheable ("unsupported source language") |

We also observed that CMake 3.28 turns on dependency scanning for *every*
C++23 target by default. It added `-fmodules-ts -fdeps-format=p1689r5` to
every GCC compile in this project, even with no modules at all. GCC 14 still
requires `-fmodules-ts`, the flag for its experimental implementation.

## Decision

- No C++20 modules in the codebase for now. The domain stays a classic
  static library with headers.
- `CMAKE_CXX_SCAN_FOR_MODULES` is `OFF` in `cmake/CompilerOptions.cmake`,
  which removes the scan step and the experimental GCC flag from every
  compile.

The toolchain *can* build a modular domain; the probe proves that. We decline
anyway, for these reasons:

1. **CI build caching.** CI relies on ccache (every job caches `.ccache`).
   With GCC, all module compiles are uncacheable, and those are the
   compiles every other target depends on.
2. **No `import std`.** Every module unit would textually include the
   standard library in its global module fragment, and every consumer
   (tests, fuzzers, benchmarks, codec) would mix `import` with textual std
   headers. That is the configuration where compiler module bugs
   concentrate, and a toy probe cannot rule them out.
3. **Enforcement.** Our architecture fitness functions (ADR-0002) scan
   `#include` directives. Modules would need import-aware rules, while the
   current guarantees already hold.
4. **Small benefit.** The domain is about a thousand lines. Build time is
   dominated by gRPC-generated code, which cannot be modularised.

## Consequences

- All tools (clang-tidy, clangd, ccache, sanitizers, libFuzzer) work without
  caveats. Builds skip the scan step.
- The domain's encapsulation comes from the architecture rules, not from
  module boundaries.
- **Revisit when:**
  - the standard library provides `import std` (libstdc++ 15 / GCC 15+);
  - ccache caches module compiles.

  At that point, run a spike that converts only the domain behind a CMake
  option, and measure build time and CI cache hit rate before adopting it.
