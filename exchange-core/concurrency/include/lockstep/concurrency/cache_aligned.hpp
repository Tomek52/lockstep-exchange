#pragma once

#include <cstddef>

namespace lockstep::concurrency {

/// Alignment used to keep independently-written data on separate cache lines.
///
/// 128 rather than 64 on purpose: Intel's L2 adjacent-line prefetcher pulls
/// cache lines in 128-byte pairs, and Apple/ARM big cores use 128-byte lines,
/// so 64-byte padding still false-shares on common hardware. We do not use
/// std::hardware_destructive_interference_size because GCC warns that its value
/// is not ABI-stable across -mtune settings (-Winterference-size). ADR-0011.
inline constexpr std::size_t cache_line_size = 128;

/// Wraps a value so that it occupies (at least) one full cache line by itself.
template <typename T>
struct alignas(cache_line_size) CacheAligned {
    T value{};
};

}  // namespace lockstep::concurrency
