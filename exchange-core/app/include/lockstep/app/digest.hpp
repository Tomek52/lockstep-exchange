#pragma once

#include <cstdint>
#include <span>

#include "lockstep/app/messages.hpp"
#include "lockstep/app/replay.hpp"
#include "lockstep/domain/order_book.hpp"

namespace lockstep::app {

/// Incrementally folds a shard's live output into a 64-bit digest, so the
/// running exchange can report (`--print-digest-on-exit`) the digest of what
/// it actually produced without re-reading its own journal back from disk -
/// re-reading would make the check a replay-equals-replay tautology that
/// cannot catch a bug in how (or whether) the live run itself folds its
/// output (task 010 review F1).
///
/// One instance per shard, owned by its ShardRuntime: add() is called once
/// per event / reply, by the shard thread only, in the exact order the
/// corresponding full-journal replay would produce them (first every record
/// resumed from an existing journal at startup, in file order, then every
/// command the live run processes, in the order it assigns sequence
/// numbers) - that ordering equality is what makes the live digest equal a
/// `lockstep-replay` run over the resulting file. finish() may be called
/// from any thread, but only after the shard thread that wrote to this
/// builder has stopped and been joined: joining a std::jthread establishes
/// a happens-before between everything the thread did and the joining
/// thread's subsequent reads, so no further synchronisation is needed here.
///
/// Canonical = independent of struct padding and host endianness: every
/// field is written out little-endian, one at a time, in declaration order,
/// never by copying a struct's raw bytes. Enum and bool fields use explicit
/// byte values, not the C++ enumerator, so reordering an enum cannot change
/// the digest. Every add() also folds a byte saying which overload it is
/// (event or reply) before the item's own fields (task 010 review n1): two
/// item types can otherwise serialize to the same total length (an ok
/// CommandReply and a RiskCommandApplied event both do, at 33 bytes), so
/// without this an event and a reply could one day collide at the type
/// level, not just by unlucky field values. `digest()` below is built on
/// top of this class, so a file replay and a live run can never fold their
/// output differently.
class DigestBuilder {
public:
    DigestBuilder() noexcept;

    // Not noexcept: folding an event dispatches on its variant via
    // std::visit, which clang-tidy's bugprone-exception-escape treats as
    // possibly throwing (std::bad_variant_access on a valueless_by_exception
    // variant) even though Event, trivially copyable, never reaches that
    // state (see digest.cpp's digest_event, which this calls through).
    void add(const PublishedEvent& event);
    void add(const CommandReply& reply) noexcept;

    /// Folds in the final book state and returns the digest. `books` must be
    /// in a stable order (by instrument id); finish() does not sort them,
    /// since the caller already has them in Router order (ADR-0004's
    /// "iterate flat_maps, or sort first" rule applies to the caller, not
    /// here). Does not mutate the builder, so finish() may be called more
    /// than once (e.g. to compare digests of two different book snapshots
    /// over the same event/reply stream).
    [[nodiscard]] std::uint64_t finish(std::span<const domain::BookSnapshot> books) const noexcept;

    /// Commands folded so far (one add(CommandReply) per command, always;
    /// matches `digest()`'s event/reply counts below).
    [[nodiscard]] std::uint64_t commands() const noexcept { return reply_count_; }
    [[nodiscard]] std::uint64_t events() const noexcept { return event_count_; }

private:
    std::uint64_t hash_;
    std::uint64_t event_count_{0};
    std::uint64_t reply_count_{0};
};

/// Order-sensitive 64-bit digest (FNV-1a over a canonical byte encoding) of
/// everything a shard produced: its events, its replies and its final book
/// state (task 010). Two runs are equivalent (ADR-0004) iff their digests
/// agree over identical inputs - this is what `lockstep-replay` prints and
/// what a live run's DigestBuilder above is built to match exactly.
///
/// Implemented on top of DigestBuilder, adding events and replies
/// interleaved per command - this command's events, then its reply, before
/// the next command's, matching a live run's own fold order (task 010
/// review F1) - then finishing with `books`, so a file-based replay (this
/// function) and a live run's incremental digest cannot diverge through
/// separately maintained folding logic.
[[nodiscard]] std::uint64_t digest(const ReplayOutput& output,
                                   std::span<const domain::BookSnapshot> books);

}  // namespace lockstep::app
