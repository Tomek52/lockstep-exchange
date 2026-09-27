# 9. Toolchain baseline and C++23 feature fallbacks

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

We want modern C++ where it genuinely helps, which means C++23 library
features that are unevenly implemented. The target baseline is what Ubuntu
24.04 ships: **GCC 14.2** and **Clang 19.1.1**, both using **libstdc++ 14**.

We did not rely on documentation. A probe program compiled on both compilers
while building the skeleton produced these results:

| Feature | Macro / probe | GCC 14 | Clang 19 (libstdc++ 14) |
|---|---|---|---|
| `std::expected` | `__cpp_lib_expected` 202211 | ✅ | ✅ |
| `std::print` | `__cpp_lib_print` 202211 | ✅ | ✅ |
| `std::generator` | `__cpp_lib_generator` 202207 | ✅ | ✅ |
| `std::move_only_function` | 202110 | ✅ | ✅ |
| `ranges::to`, `views::zip/chunk_by/enumerate` | ✅ | ✅ | ✅ |
| `if consteval`, `std::unreachable` | ✅ | ✅ | ✅ |
| deducing `this` | `__cpp_explicit_this_parameter` | ✅ 202110 | ✅ works, but **macro not defined** |
| `std::stacktrace` | 202011 | ✅ needs **`-lstdc++exp`** | ✅ needs **`-lstdc++exp`** |
| `std::flat_map` | `__cpp_lib_flat_map` | ❌ (arrives in libstdc++ 15) | ❌ |

## Decision

1. **Baseline:** GCC 14+ or Clang 19+ with libstdc++ 14+. Presets pin
   `g++-14` (debug/release) and `clang++-19` (clang-debug, asan-ubsan, tsan).
2. **Probe, don't trust macros.** `cmake/FeatureProbe.cmake` compiles a small
   program per required feature (`check_cxx_source_compiles`). Configure
   fails with a readable message naming this ADR if the toolchain is too old.
   Deducing `this` is probed by compiling, because Clang 19 implements it
   without advertising the macro.
3. **`std::stacktrace`:** the probe tries without an extra library, then with
   `stdc++exp`, and exposes the result as the `lockstep::stacktrace` INTERFACE
   target.
4. **`std::flat_map`:** `lockstep/domain/flat_map.hpp` selects `std::flat_map`
   when `__cpp_lib_flat_map >= 202207L`, else
   `detail::sorted_vector_map`, our fallback. The fallback:
   - uses the same layout as the standard one: sorted parallel key and value
     vectors;
   - has the same proxy-reference iteration (`pair<const K&, V&>`);
   - implements only the subset we use.

   Callers follow the portability rules in the header, for example binding
   with `auto&& [k, v]`, never `auto&`. Switching to a newer standard library
   then needs no code change. The configure summary prints which
   implementation is active.
5. **Where each feature is used, and why:**
   - `std::expected`: domain and codec results (ADR-0008).
   - `std::flat_map`: price levels and per-shard book index. Few keys, hot
     lookups near the top of book; a benchmark against `std::map` lives in
     `bench/`.
   - `std::generator`: lazy journal reading (task 009).
   - Deducing `this`: the `Additive` mixin for strong types (no CRTP) and
     const/non-const deduplication (`OrderBook::with_side`, `flat_map`
     fallback).
   - `std::move_only_function`: completions carrying move-only gRPC state.
   - `std::print`: logging and the fatal handler (never on the hot path).
   - `ranges::to`, `views::enumerate/zip/chunk_by`: router assignment,
     snapshots, determinism diffing.
   - `std::stacktrace`: fatal and terminate handlers.
   - `std::unreachable`: after exhaustive switches.
   - `if consteval`: journal byte I/O. Shifts during constant evaluation (so
     format tests are `static_assert`s); `memcpy` at runtime.
   - Concepts: `DomainEvent`, `ConcurrentQueue`/`MultiProducerQueue`,
     `IdleStrategy`.

## Consequences

- The flat_map fallback is our code and must be tested as such
  (`flat_map_test.cpp`, including a `bidirectional_iterator` concept check).
  CI cannot yet exercise the `std::flat_map` branch; that happens
  automatically when the baseline moves to libstdc++ 15.
- A toolchain regression shows up as a configure-time error, not as a
  cryptic template failure deep in the build.
- Features are adopted where they fit. `std::mdspan`, for example, is not
  used because nothing here is a multidimensional array.
