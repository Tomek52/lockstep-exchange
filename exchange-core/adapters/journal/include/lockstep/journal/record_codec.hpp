#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include "lockstep/app/ports/journal.hpp"
#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/risk_state.hpp"
#include "lockstep/domain/shard_engine.hpp"
#include "lockstep/domain/types.hpp"
#include "lockstep/journal/byte_io.hpp"

namespace lockstep::journal {

// Record payload encoding, format version 1 (ADR-0012, task 008):
//
//   sequence u64 | timestamp i64 | tag u8 | fields...
//
// Fields follow in declaration order of the command struct, little-endian.
// Strong types are stored as their representation; enums and bools take one
// byte each, with the explicit values below (never the C++ enumerator value,
// so reordering an enum cannot change the format).
//
//   NewOrder        trader u64 | client_order_id u64 | instrument u32 | side u8
//                   | type u8 | time_in_force u8 | price i64 | quantity u64
//   CancelOrder     trader u64 | instrument u32 | order_id u64
//   ModifyOrder     trader u64 | instrument u32 | order_id u64 | new_price i64
//                   | new_quantity u64
//   BlockTrader     command_id u64 | trader u64
//   UnblockTrader   command_id u64 | trader u64
//   KillSwitch      command_id u64 | engaged u8
//   RiskLinkStatus  connected u8
//
//   side: Buy 0, Sell 1 · type: Limit 0, Market 1 · time_in_force: Gtc 0, Ioc 1
//   bool: 0 or 1
//
// A payload must be consumed exactly: trailing bytes are Corrupt.

/// Appends the payload encoding of `command` to `out` (no record header).
void encode_payload(const domain::SequencedCommand& command, std::vector<std::byte>& out);

/// Decodes one payload. Total: any byte sequence yields either a command or
/// JournalError::Corrupt (truncated fields, unknown tag, invalid enum or bool
/// byte, trailing bytes); never undefined behaviour.
[[nodiscard]] std::expected<domain::SequencedCommand, app::JournalError> decode_payload(
    std::span<const std::byte> payload) noexcept;

namespace detail {

inline constexpr std::uint64_t fnv1a_offset_basis = 0xCBF2'9CE4'8422'2325ULL;
inline constexpr std::uint64_t fnv1a_prime = 0x0000'0100'0000'01B3ULL;

template <std::integral T>
[[nodiscard]] constexpr std::uint64_t fnv1a_le(std::uint64_t hash, T value) noexcept {
    std::array<std::byte, sizeof(T)> bytes{};
    store_le(value, std::span{bytes});
    for (const std::byte byte : bytes) {
        hash = (hash ^ std::to_integer<std::uint64_t>(byte)) * fnv1a_prime;
    }
    return hash;
}

[[nodiscard]] constexpr std::uint8_t policy_byte(domain::RiskLinkPolicy policy) noexcept {
    switch (policy) {
        case domain::RiskLinkPolicy::FailOpen:
            return 0;
        case domain::RiskLinkPolicy::FailClosed:
            return 1;
    }
    std::unreachable();
}

}  // namespace detail

/// FNV-1a over every ShardConfig input that affects ShardEngine output, for
/// FileHeader::config_hash (ADR-0017): the spec count (u32), each spec sorted
/// by id (id u32, min_price i64, max_price i64, max_order_quantity u64), then
/// the risk link policy (FailOpen 0, FailClosed 1). shard id and count are not
/// hashed; they have their own header fields. Changing any of this bumps
/// format_version.
[[nodiscard]] constexpr std::uint64_t config_hash(const domain::ShardConfig& config) noexcept {
    // Startup-only: the copy keeps the hash independent of config order.
    std::vector<domain::InstrumentSpec> specs = config.instruments;
    std::ranges::sort(specs, {}, &domain::InstrumentSpec::id);

    std::uint64_t hash = detail::fnv1a_offset_basis;
    hash = detail::fnv1a_le(hash, static_cast<std::uint32_t>(specs.size()));
    for (const domain::InstrumentSpec& spec : specs) {
        hash = detail::fnv1a_le(hash, spec.id.value());
        hash = detail::fnv1a_le(hash, spec.min_price.value());
        hash = detail::fnv1a_le(hash, spec.max_price.value());
        hash = detail::fnv1a_le(hash, spec.max_order_quantity.value());
    }
    return detail::fnv1a_le(hash, detail::policy_byte(config.risk_link_policy));
}

}  // namespace lockstep::journal
