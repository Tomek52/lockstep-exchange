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

// Every `.size()` below is explicitly widened to std::uint64_t (brace-init:
// static_cast<std::uint64_t> is flagged -Wuseless-cast on this toolchain,
// where std::size_t already is std::uint64_t) so the fold always writes 8
// bytes regardless of platform, rather than silently narrowing on a 32-bit
// std::size_t.
std::uint64_t digest_book(std::uint64_t hash, const domain::BookSnapshot& book) noexcept {
    hash = fnv1a_le(hash, book.instrument.value());
    hash = fnv1a_le(hash, std::uint64_t{book.bids.size()});
    for (const domain::LevelView& level : book.bids) {
        hash = digest_level(hash, level);
    }
    hash = fnv1a_le(hash, std::uint64_t{book.asks.size()});
    for (const domain::LevelView& level : book.asks) {
        hash = digest_level(hash, level);
    }
    return hash;
}

}  // namespace

DigestBuilder::DigestBuilder() noexcept : hash_{fnv1a_offset_basis} {}

void DigestBuilder::add(const PublishedEvent& event) noexcept {
    hash_ = digest_published_event(hash_, event);
    ++event_count_;
}

void DigestBuilder::add(const CommandReply& reply) noexcept {
    hash_ = digest_reply(hash_, reply);
    ++reply_count_;
}

std::uint64_t DigestBuilder::finish(std::span<const domain::BookSnapshot> books) const noexcept {
    // Counts folded in here, after every item (rather than as length
    // prefixes before each section): a streaming builder cannot know its
    // final counts up front. Folding them anywhere deterministic still
    // tells apart a trailing empty/missing section and a boundary that
    // moved (e.g. one shard's event list one shorter, its reply list one
    // longer) - what matters is that every run folds counts at the same
    // point, which add()/finish() always do.
    std::uint64_t hash = hash_;
    hash = fnv1a_le(hash, event_count_);
    hash = fnv1a_le(hash, reply_count_);
    hash = fnv1a_le(hash, std::uint64_t{books.size()});
    for (const domain::BookSnapshot& book : books) {
        hash = digest_book(hash, book);
    }
    return hash;
}

std::uint64_t digest(const ReplayOutput& output, std::span<const domain::BookSnapshot> books) {
    DigestBuilder builder;
    // Interleaved per command - this command's events, then its reply,
    // before the next command's - not all events followed by all replies:
    // a live run's ShardRuntime folds its DigestBuilder that way (it only
    // ever has one command's output in hand at a time), and DigestBuilder's
    // fold is order-sensitive to more than just within-category order, so
    // matching that interleaving exactly is what makes this function agree
    // with a live run's digest (task 010 review F1 found this the hard way:
    // folding all events then all replies gave a different, wrong answer).
    //
    // app::replay() guarantees exactly one reply per command in ascending
    // sequence order, with every event carrying its command's sequence, so
    // walking the events in lockstep and flushing every event whose
    // sequence is at or before the next reply's groups each reply with its
    // own events before moving on. Any events left over after the last
    // reply (never produced by app::replay(), but this function's input is
    // a plain struct, not a sealed one) are still folded, in order, at the
    // end, rather than silently dropped.
    std::size_t event_index = 0;
    for (const CommandReply& reply : output.replies) {
        while (event_index < output.events.size() &&
               output.events[event_index].sequence.value() <= reply.sequence.value()) {
            builder.add(output.events[event_index]);
            ++event_index;
        }
        builder.add(reply);
    }
    while (event_index < output.events.size()) {
        builder.add(output.events[event_index]);
        ++event_index;
    }
    return builder.finish(books);
}

}  // namespace lockstep::app
