#include "lockstep/app/subscription.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "lockstep/domain/events.hpp"

namespace lockstep::app {

namespace {

enum class EventCategory : std::uint8_t { Trade, BookUpdate, Private, Always };

// Ordinary (non-template) function: safe to switch on EventKind directly,
// unlike a per-alternative visitor - see instrument_of()'s comment for why
// that one cannot do the same.
EventCategory category_of(domain::EventKind kind) {
    switch (kind) {
        case domain::EventKind::Trade:
            return EventCategory::Trade;
        case domain::EventKind::BookLevelChanged:
            return EventCategory::BookUpdate;
        case domain::EventKind::InstrumentStatusChanged:
            // Always delivered, subject only to the instrument filter - see
            // SubscriptionFilter's doc and docs/tasks/011-publisher-fanout.md.
            return EventCategory::Always;
        case domain::EventKind::OrderAccepted:
        case domain::EventKind::OrderCancelled:
        case domain::EventKind::OrderModified:
        case domain::EventKind::RiskCommandApplied:
            return EventCategory::Private;
    }
    std::unreachable();
}

domain::EventKind kind_of(const domain::Event& event) {
    // Every DomainEvent alternative carries its own `kind` (events.hpp's
    // DomainEvent concept requires it), so this is exhaustive without
    // needing a per-alternative branch here.
    return std::visit([](const auto& payload) { return payload.kind; }, event);
}

std::optional<domain::InstrumentId> instrument_of(const domain::Event& event) {
    return std::visit(
        []<typename T>(const T& payload) -> std::optional<domain::InstrumentId> {
            if constexpr (std::is_same_v<T, domain::OrderAccepted> ||
                          std::is_same_v<T, domain::OrderCancelled> ||
                          std::is_same_v<T, domain::OrderModified> ||
                          std::is_same_v<T, domain::Trade> ||
                          std::is_same_v<T, domain::BookLevelChanged> ||
                          std::is_same_v<T, domain::InstrumentStatusChanged>) {
                return payload.instrument;
            } else if constexpr (std::is_same_v<T, domain::RiskCommandApplied>) {
                // Per-shard broadcast ack, not scoped to one book: never
                // excluded by an instrument filter.
                return std::nullopt;
            } else {
                // A new Event alternative with no case above: fail to
                // compile rather than silently gaining no instrument filter
                // (task 011 code review). static_assert must depend on T,
                // or it would fire for every instantiation, not just this
                // (would-be) unreachable one.
                static_assert(!std::is_same_v<T, T>,
                              "unhandled Event alternative in instrument_of");
                return std::nullopt;
            }
        },
        event);
}

}  // namespace

Subscription::Subscription(SubscriptionFilter filter, std::size_t capacity)
    : filter_{std::move(filter)}, ring_{capacity} {}

Subscription::~Subscription() {
    // No concurrent access during destruction: a Subscription is only
    // destroyed once both the consumer and the publisher have dropped their
    // shared_ptr to it (see the class comment), so on_ready() and
    // notify_ready() cannot race this load.
    delete on_ready_.load(std::memory_order_relaxed);
}

std::size_t Subscription::poll(std::span<PublishedEvent> out) {
    std::size_t count = 0;
    while (count < out.size()) {
        std::optional<PublishedEvent> popped = ring_.try_pop();
        if (!popped) {
            break;
        }
        out[count++] = *popped;
    }
    return count;
}

bool Subscription::overflowed() const noexcept {
    // acquire: pairs with the release in deliver()/close(), so a consumer
    // that observes overflowed()==true also observes every event already
    // published into the ring before that point (SpscQueue's own
    // acquire/release already orders the ring's contents independently, but
    // close() has no ring push to carry that ordering, so this flag has to
    // provide it itself for that path).
    return overflowed_.load(std::memory_order_acquire);
}

void Subscription::cancel() noexcept {
    // seq_cst: paired with notify_ready()'s Dekker handshake - see the
    // notifying_ member's comment for why acquire/release is not enough
    // across two independent atomics.
    cancelled_.store(true, std::memory_order_seq_cst);
    while (notifying_.load(std::memory_order_seq_cst)) {
        // A short spin, not a park: notify_ready() never blocks (ADR-0006),
        // so this window is at most one on_ready() callback's run time.
        std::this_thread::yield();
    }
}

bool Subscription::cancelled() const noexcept {
    return cancelled_.load(std::memory_order_seq_cst);  // seq_cst: see cancel()
}

bool Subscription::on_ready(std::move_only_function<void() noexcept> callback) {
    auto* owned = new std::move_only_function<void() noexcept>(std::move(callback));
    std::move_only_function<void() noexcept>* expected = nullptr;
    // Set-once: compare_exchange only succeeds the first time. A second
    // call's object was never published, so freeing it here is always safe
    // - unlike an unconditional exchange()+delete, which could free a
    // function object the publisher thread is still inside (notify_ready()).
    //
    // The CAS and the fence right after it are the consumer-side half of a
    // fence-to-fence argument with notify_ready() (the publisher side: a
    // seq_cst fence right before its hook load - see that function's
    // comment). The racing publisher-side write is deliver()'s release
    // store to the ring's tail_ (SpscQueue::try_push), not anything on this
    // object. A plain seq_cst CAS here and a plain seq_cst load there would
    // only order on_ready_ itself between the two threads - the C++ memory
    // model does not extend that to a *different* object (the ring's
    // tail_, read with only an acquire load by this consumer's next
    // poll()) without an explicit fence on both sides naming the same
    // seq_cst total order (this matters on weaker-than-x86 models, e.g.
    // AArch64/RCPC). With both fences in place: if notify_ready() reads
    // this CAS's result after its own install-order position (on_ready_
    // visibly non-null to it), its fence
    // guarantees it also sees deliver()'s tail_ store; if it reads before
    // (on_ready_ still null to it, so it skips the hook), this fence
    // guarantees the consumer's very next poll() - happening after this
    // call returns - sees that same tail_ store instead. Either way the
    // data is never stuck invisible to both sides at once.
    if (on_ready_.compare_exchange_strong(expected, owned, std::memory_order_seq_cst,
                                          std::memory_order_seq_cst)) {
        // seq_cst: see above.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return true;
    }
    delete owned;
    return false;
}

void Subscription::set_start_sequence(std::vector<domain::SequenceNumber> cutoffs) {
    start_sequence_ = std::move(cutoffs);
    // release: pairs with the acquire in is_registered(), publishing
    // start_sequence_ (just written, same thread) before a waiting
    // subscribe() can observe this flip and act on it.
    registered_.store(true, std::memory_order_release);
}

bool Subscription::is_registered() const noexcept {
    return registered_.load(std::memory_order_acquire);  // acquire: see set_start_sequence()
}

bool Subscription::wants(const PublishedEvent& event) const {
    const std::size_t shard = event.shard.value();
    // Registered after this event's command had already started (or not
    // registered at all, if called before set_start_sequence() - should
    // never happen, see its doc): never deliver it, to avoid ever handing a
    // subscription only part of one command's events (see the class
    // comment).
    if (shard >= start_sequence_.size() || event.sequence <= start_sequence_[shard]) {
        return false;
    }
    if (!filter_.instruments.empty()) {
        const std::optional<domain::InstrumentId> instrument = instrument_of(event.event);
        if (instrument.has_value() &&
            std::ranges::find(filter_.instruments, *instrument) == filter_.instruments.end()) {
            return false;
        }
    }
    switch (category_of(kind_of(event.event))) {
        case EventCategory::Trade:
            return filter_.trades;
        case EventCategory::BookUpdate:
            return filter_.book_updates;
        case EventCategory::Private:
            return filter_.private_events;
        case EventCategory::Always:
            return true;
    }
    std::unreachable();
}

bool Subscription::deliver(const PublishedEvent& event) {
    if (ring_.try_push(PublishedEvent{event})) {
        return true;
    }
    // ADR-0006's slow-consumer policy: a full ring means this consumer isn't
    // keeping up. Stop delivering to it instead of blocking the publisher -
    // and transitively every shard (ADR-0003) - on a slow reader.
    // release: pairs with the acquire in overflowed(); see its comment.
    overflowed_.store(true, std::memory_order_release);
    return false;
}

void Subscription::notify_ready() noexcept {
    // No "on_ready_ still null, nothing to do" fast path here on purpose: a
    // relaxed pre-check of on_ready_ can read a stale null long enough that
    // this flush - the only one racing a concurrent on_ready() install -
    // returns without ever calling the hook, the exact store-buffering lost
    // wake-up on_ready()'s seq_cst sequence exists to rule out. Every call
    // pays the seq_cst sequence below.
    notifying_.store(true,
                     std::memory_order_seq_cst);  // seq_cst: see the notifying_ member's comment
    // Re-checked here (not just by the caller) so a cancel() that lands
    // exactly between deliver_to()'s own cancelled() check and this call
    // still gets "the hook never runs again after cancel() returns" -
    // cancel()'s spin only starts blocking once notifying_ is visibly true,
    // which happens-before this load (seq_cst on both sides).
    if (!cancelled_.load(std::memory_order_seq_cst)) {
        // The publisher-side half of on_ready()'s fence-to-fence argument -
        // see its comment for the full pairing. This fence sits right
        // before the hook load below, so it captures deliver()'s release
        // store to the ring's tail_ (this flush's own delivery, already
        // done by the time notify_ready() runs) on the same side as the
        // on_ready_ load that decides whether to call the hook.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto* callback = on_ready_.load(std::memory_order_seq_cst);
        if (callback != nullptr) {
            (*callback)();
        }
    }
    notifying_.store(false, std::memory_order_seq_cst);
}

void Subscription::close() noexcept {
    // release: see deliver()'s identical comment on overflowed_.
    overflowed_.store(true, std::memory_order_release);
    // release: see set_start_sequence()'s identical comment - a subscribe()
    // call that is only waiting for is_registered() (never actually gets a
    // start_sequence_ stamp from drain_control() in this path) still needs
    // to unblock.
    registered_.store(true, std::memory_order_release);
    notify_ready();
}

}  // namespace lockstep::app
