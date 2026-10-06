#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <stop_token>
#include <utility>
#include <vector>

#include "lockstep/app/digest.hpp"
#include "lockstep/app/messages.hpp"
#include "lockstep/app/ports/clock.hpp"
#include "lockstep/app/ports/journal.hpp"
#include "lockstep/app/queues.hpp"
#include "lockstep/concurrency/cache_aligned.hpp"
#include "lockstep/concurrency/idle_strategy.hpp"
#include "lockstep/domain/events.hpp"
#include "lockstep/domain/shard_engine.hpp"

namespace lockstep::app {

/// Runtime counters for one shard (task 007). Diagnostic only - nothing in
/// the runtime depends on these values, so stats() reads them with relaxed
/// atomics and may be called from any thread at any time, including while
/// the shard thread is running (ADR-0011: weakest ordering that is correct).
/// Each field is individually consistent (its own atomic load), but the four
/// are not a joint snapshot while the shard is running: it can update one
/// counter between two of this call's loads, so e.g. `batches * max_batch`
/// is not guaranteed to bound `commands` exactly until the shard has
/// stopped and joined. Call after Engine::stop() for an exact read.
struct ShardStats {
    std::uint64_t commands{0};
    std::uint64_t batches{0};
    std::uint64_t max_batch{0};
    std::uint64_t parks{0};
};

/// One shard: the only thread that touches its ShardEngine (ADR-0003).
///
/// Loop: drain up to `max_batch` commands from ingress; for each, assign the
/// next sequence number and a timestamp, append it to the journal, apply it,
/// and stage its events and reply. Then commit the journal and only then push
/// the staged items to egress - outputs never overtake durability (ADR-0004).
class ShardRuntime {
public:
    struct Config {
        domain::ShardConfig shard;
        std::size_t ingress_capacity{4096};
        std::size_t egress_capacity{16384};
        std::size_t max_batch{256};
        // See EngineConfig::record_digest's comment (task 010 review M1).
        bool record_digest{false};
    };

    ShardRuntime(const Config& config,
                 std::unique_ptr<Journal> journal,
                 Clock& clock,
                 concurrency::Doorbell& publisher_doorbell);

    [[nodiscard]] IngressQueue& ingress() noexcept { return ingress_; }
    [[nodiscard]] EgressQueue& egress() noexcept { return egress_; }
    [[nodiscard]] domain::ShardId shard() const noexcept { return engine_.shard(); }

    /// Rung by Engine::submit/broadcast after a successful push to ingress(),
    /// so this shard's parked thread (if any) wakes to process it.
    [[nodiscard]] concurrency::Doorbell& doorbell() noexcept { return doorbell_; }

    /// Rebuilds state from a journal that already held commands, before the
    /// shard thread starts (ADR-0020): replays `commands` into the engine
    /// exactly as the original run did, and resumes sequence numbering after
    /// the last one. Does not publish their events/replies anywhere (they
    /// already reached their recipients in the run that produced them, so
    /// republishing them would be new, phantom activity, not state rebuild),
    /// but when digest recording is enabled (`Config::record_digest`) it
    /// does fold each one into digest_builder(), in file order, exactly as a
    /// full-journal replay would: this is what makes the live digest equal
    /// a lockstep-replay run over the resulting file (task 010 review F1),
    /// including the part of the file this run did not itself produce. Must
    /// be called before run(), from the owner thread only, and at most once
    /// (it does not clear any state of its own, only engine_'s and, if
    /// enabled, digest_'s).
    template <std::ranges::input_range Commands>
        requires std::convertible_to<std::ranges::range_reference_t<Commands>,
                                     const domain::SequencedCommand&>
    void resume_from_journal(Commands&& commands) {
        domain::EventBuffer scratch;
        for (const domain::SequencedCommand& command : std::forward<Commands>(commands)) {
            scratch.clear();
            const domain::CommandResult result = engine_.apply(command, scratch);
            if (digest_) {
                for (const domain::Event& event : scratch.events()) {
                    digest_->add(
                        PublishedEvent{shard(), command.sequence, command.timestamp, event});
                }
                digest_->add(CommandReply{shard(), command.sequence, command.timestamp, result});
            }
            sequence_ = command.sequence.value();
        }
    }

    /// Thread body. Returns after `stop` is requested and ingress is drained.
    /// Precondition for a lossless stop: producers have stopped submitting.
    void run(const std::stop_token& stop);

    /// Only valid while the shard thread is not running (tests, replay checks).
    [[nodiscard]] const domain::ShardEngine& engine() const noexcept { return engine_; }

    /// Safe to call from any thread, whether or not the shard thread is
    /// running; see the ShardStats comment.
    [[nodiscard]] ShardStats stats() const noexcept;

    /// nullopt unless `Config::record_digest` was set (task 010 review M1:
    /// folding has a measurable per-order cost, see ADR-0020, so it stays
    /// off unless something wants the digest). When present,
    /// holds every event and reply this shard has folded so far - every
    /// record resumed from an existing journal at startup, then every
    /// command this run has processed, in order (task 010 review F1).
    /// Written by exactly one thread at a time, but not always the same
    /// one: the *owner* thread (whoever constructs this ShardRuntime) writes
    /// it via resume_from_journal(), before any shard thread exists; the
    /// *shard* thread writes it via process() from then on. The owner
    /// thread's writes happen-before the shard thread's first read/write of
    /// it too, because starting a std::jthread happens-after everything its
    /// starting thread did beforehand - the same guarantee that, at the
    /// other end, lets any thread read this safely after Engine::stop()
    /// joins the shard thread.
    [[nodiscard]] const std::optional<DigestBuilder>& digest_builder() const noexcept {
        return digest_;
    }

private:
    std::size_t poll_once();
    void process(InboundCommand inbound);
    void release_staged();

    domain::ShardEngine engine_;
    std::unique_ptr<Journal> journal_;
    Clock& clock_;
    std::size_t max_batch_;
    IngressQueue ingress_;
    EgressQueue egress_;
    // alignas on both doorbell_ and publisher_doorbell_: gRPC threads ring
    // doorbell_ on every submit/broadcast, so without separating it from
    // the shard thread's own events_/staged_/sequence_ (and from the
    // publisher_doorbell_ reference right after it) that cross-thread write
    // would false-share a cache line with state only this thread touches
    // (ADR-0011's cache-line rule).
    alignas(concurrency::cache_line_size) concurrency::Doorbell doorbell_;
    alignas(concurrency::cache_line_size) concurrency::Doorbell& publisher_doorbell_;
    domain::EventBuffer events_;
    std::vector<OutboundItem> staged_;
    std::uint64_t sequence_{0};
    std::optional<DigestBuilder> digest_;

    // Written only by the shard thread; read from any thread via stats().
    std::atomic<std::uint64_t> commands_stat_{0};
    std::atomic<std::uint64_t> batches_stat_{0};
    std::atomic<std::uint64_t> max_batch_stat_{0};
    std::atomic<std::uint64_t> parks_stat_{0};
};

}  // namespace lockstep::app
