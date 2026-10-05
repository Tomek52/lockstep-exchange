#include "lockstep/app/digest.hpp"

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>

namespace lockstep::app {

namespace {

inline constexpr std::uint64_t fnv1a_offset_basis = 0xCBF2'9CE4'8422'2325ULL;
inline constexpr std::uint64_t fnv1a_prime = 0x0000'0100'0000'01B3ULL;

/// Folds `value` into `hash` byte by byte, least-significant first, so the
/// result is the same on a big-endian host: no std::memcpy of a struct or an
/// integer, only explicit shifts (see digest.hpp's canonical-encoding note).
template <std::integral T>
[[nodiscard]] constexpr std::uint64_t fnv1a_le(std::uint64_t hash, T value) noexcept {
    using U = std::make_unsigned_t<T>;
    const auto bits = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        const auto byte = static_cast<std::uint8_t>(bits >> (8U * i));
        hash = (hash ^ byte) * fnv1a_prime;
    }
    return hash;
}

[[nodiscard]] constexpr std::uint64_t fnv1a_bool(std::uint64_t hash, bool value) noexcept {
    return fnv1a_le(hash, static_cast<std::uint8_t>(value ? 1 : 0));
}

// Explicit byte values for every enum this file digests, independent of the
// enumerators' C++ values: reordering an enum in events.hpp/types.hpp/
// reject_reason.hpp cannot silently change an existing digest.

[[nodiscard]] constexpr std::uint8_t byte_of(domain::EventKind kind) noexcept {
    switch (kind) {
        case domain::EventKind::OrderAccepted:
            return 0;
        case domain::EventKind::OrderCancelled:
            return 1;
        case domain::EventKind::OrderModified:
            return 2;
        case domain::EventKind::Trade:
            return 3;
        case domain::EventKind::BookLevelChanged:
            return 4;
        case domain::EventKind::InstrumentStatusChanged:
            return 5;
        case domain::EventKind::RiskCommandApplied:
            return 6;
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::uint8_t byte_of(domain::Side side) noexcept {
    switch (side) {
        case domain::Side::Buy:
            return 0;
        case domain::Side::Sell:
            return 1;
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::uint8_t byte_of(domain::OrderType type) noexcept {
    switch (type) {
        case domain::OrderType::Limit:
            return 0;
        case domain::OrderType::Market:
            return 1;
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::uint8_t byte_of(domain::CancelReason reason) noexcept {
    switch (reason) {
        case domain::CancelReason::UserRequested:
            return 0;
        case domain::CancelReason::ImmediateOrCancel:
            return 1;
        case domain::CancelReason::TraderBlocked:
            return 2;
        case domain::CancelReason::KillSwitch:
            return 3;
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::uint8_t byte_of(domain::RejectReason reason) noexcept {
    switch (reason) {
        case domain::RejectReason::UnknownInstrument:
            return 0;
        case domain::RejectReason::InvalidPrice:
            return 1;
        case domain::RejectReason::InvalidQuantity:
            return 2;
        case domain::RejectReason::TraderBlocked:
            return 3;
        case domain::RejectReason::TradingHalted:
            return 4;
        case domain::RejectReason::UnknownOrder:
            return 5;
        case domain::RejectReason::NotOrderOwner:
            return 6;
        case domain::RejectReason::DuplicateClientOrderId:
            return 7;
        case domain::RejectReason::RiskUnavailable:
            return 8;
    }
    std::unreachable();
}

/// One event alternative's fields, in declaration order. The EventKind byte
/// itself is folded in by the caller, once per event, before dispatching here.
std::uint64_t digest_fields(std::uint64_t hash, const domain::OrderAccepted& e) noexcept {
    hash = fnv1a_le(hash, e.order_id.value());
    hash = fnv1a_le(hash, e.trader.value());
    hash = fnv1a_le(hash, e.client_order_id.value());
    hash = fnv1a_le(hash, e.instrument.value());
    hash = fnv1a_le(hash, byte_of(e.side));
    hash = fnv1a_le(hash, byte_of(e.type));
    hash = fnv1a_le(hash, e.price.value());
    return fnv1a_le(hash, e.quantity.value());
}

std::uint64_t digest_fields(std::uint64_t hash, const domain::OrderCancelled& e) noexcept {
    hash = fnv1a_le(hash, e.order_id.value());
    hash = fnv1a_le(hash, e.trader.value());
    hash = fnv1a_le(hash, e.instrument.value());
    hash = fnv1a_le(hash, e.cancelled_quantity.value());
    return fnv1a_le(hash, byte_of(e.reason));
}

std::uint64_t digest_fields(std::uint64_t hash, const domain::OrderModified& e) noexcept {
    hash = fnv1a_le(hash, e.order_id.value());
    hash = fnv1a_le(hash, e.trader.value());
    hash = fnv1a_le(hash, e.instrument.value());
    hash = fnv1a_le(hash, e.price.value());
    hash = fnv1a_le(hash, e.quantity.value());
    return fnv1a_bool(hash, e.kept_priority);
}

std::uint64_t digest_fields(std::uint64_t hash, const domain::Trade& e) noexcept {
    hash = fnv1a_le(hash, e.instrument.value());
    hash = fnv1a_le(hash, e.price.value());
    hash = fnv1a_le(hash, e.quantity.value());
    hash = fnv1a_le(hash, byte_of(e.aggressor_side));
    hash = fnv1a_le(hash, e.maker_order.value());
    hash = fnv1a_le(hash, e.maker_trader.value());
    hash = fnv1a_le(hash, e.taker_order.value());
    return fnv1a_le(hash, e.taker_trader.value());
}

std::uint64_t digest_fields(std::uint64_t hash, const domain::BookLevelChanged& e) noexcept {
    hash = fnv1a_le(hash, e.instrument.value());
    hash = fnv1a_le(hash, byte_of(e.side));
    hash = fnv1a_le(hash, e.price.value());
    return fnv1a_le(hash, e.quantity.value());
}

std::uint64_t digest_fields(std::uint64_t hash, const domain::InstrumentStatusChanged& e) noexcept {
    hash = fnv1a_le(hash, e.instrument.value());
    return fnv1a_bool(hash, e.halted);
}

std::uint64_t digest_fields(std::uint64_t hash, const domain::RiskCommandApplied& e) noexcept {
    return fnv1a_le(hash, e.command_id.value());
}

// Not noexcept, unlike its digest_fields helpers: std::visit's own dispatch
// can throw std::bad_variant_access on a valueless_by_exception variant, a
// state Event (trivially copyable, ADR-0004) never reaches, but clang-tidy's
// bugprone-exception-escape cannot see through std::visit's implementation
// to know that.
std::uint64_t digest_event(std::uint64_t hash, const domain::Event& event) {
    return std::visit(
        [hash](const auto& alternative) noexcept {
            std::uint64_t h = fnv1a_le(hash, byte_of(alternative.kind));
            return digest_fields(h, alternative);
        },
        event);
}

std::uint64_t digest_result(std::uint64_t hash, const domain::CommandResult& result) noexcept {
    if (result.has_value()) {
        hash = fnv1a_le(hash, std::uint8_t{0});  // ok
        return fnv1a_le(hash, result->order_id.value());
    }
    hash = fnv1a_le(hash, std::uint8_t{1});  // rejected
    return fnv1a_le(hash, byte_of(result.error()));
}

// Not noexcept: see digest_event's comment above, which this calls.
std::uint64_t digest_published_event(std::uint64_t hash, const PublishedEvent& published) {
    hash = fnv1a_le(hash, published.shard.value());
    hash = fnv1a_le(hash, published.sequence.value());
    hash = fnv1a_le(hash, published.timestamp.value());
    return digest_event(hash, published.event);
}

std::uint64_t digest_reply(std::uint64_t hash, const CommandReply& reply) noexcept {
    hash = fnv1a_le(hash, reply.shard.value());
    hash = fnv1a_le(hash, reply.sequence.value());
    hash = fnv1a_le(hash, reply.timestamp.value());
    return digest_result(hash, reply.result);
}

std::uint64_t digest_level(std::uint64_t hash, const domain::LevelView& level) noexcept {
    hash = fnv1a_le(hash, level.price.value());
    hash = fnv1a_le(hash, level.quantity.value());
    return fnv1a_le(hash, level.order_count);
}

std::uint64_t digest_book(std::uint64_t hash, const domain::BookSnapshot& book) noexcept {
    hash = fnv1a_le(hash, book.instrument.value());
    hash = fnv1a_le(hash, static_cast<std::uint32_t>(book.bids.size()));
    for (const domain::LevelView& level : book.bids) {
        hash = digest_level(hash, level);
    }
    hash = fnv1a_le(hash, static_cast<std::uint32_t>(book.asks.size()));
    for (const domain::LevelView& level : book.asks) {
        hash = digest_level(hash, level);
    }
    return hash;
}

}  // namespace

std::uint64_t digest(const ReplayOutput& output, std::span<const domain::BookSnapshot> books) {
    std::uint64_t hash = fnv1a_offset_basis;

    // Length prefixes before each section: without them, a trailing empty
    // section and a missing one would hash identically, and two sections
    // whose boundary moved (e.g. an event list one shorter, a reply list one
    // longer) could coincidentally hash the same byte stream.
    hash = fnv1a_le(hash, output.events.size());
    for (const PublishedEvent& event : output.events) {
        hash = digest_published_event(hash, event);
    }
    hash = fnv1a_le(hash, output.replies.size());
    for (const CommandReply& reply : output.replies) {
        hash = digest_reply(hash, reply);
    }
    hash = fnv1a_le(hash, books.size());
    for (const domain::BookSnapshot& book : books) {
        hash = digest_book(hash, book);
    }
    return hash;
}

}  // namespace lockstep::app
