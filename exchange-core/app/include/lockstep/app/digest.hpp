#pragma once

#include <cstdint>
#include <span>

#include "lockstep/app/replay.hpp"
#include "lockstep/domain/order_book.hpp"

namespace lockstep::app {

/// Order-sensitive 64-bit digest (FNV-1a over a canonical byte encoding) of
/// everything a shard produced: its events, its replies and its final book
/// state (task 010). Two runs are equivalent (ADR-0004) iff their digests
/// agree over identical inputs - this is what `lockstep-replay` and
/// `--print-digest-on-exit` print and compare.
///
/// Canonical = independent of struct padding and host endianness: every field
/// is written out little-endian, one at a time, in declaration order, never
/// by copying a struct's raw bytes (which would pull in padding and the
/// platform's native layout). Enum and bool fields use explicit byte values,
/// not the C++ enumerator, so reordering an enum cannot change the digest.
///
/// `books` must be in a stable order (by instrument id); `digest()` does not
/// sort them, since the caller already has them in Router order (ADR-0004's
/// "iterate flat_maps, or sort first" rule applies to the caller, not here).
[[nodiscard]] std::uint64_t digest(const ReplayOutput& output,
                                   std::span<const domain::BookSnapshot> books);

}  // namespace lockstep::app
