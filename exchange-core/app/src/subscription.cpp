#include "lockstep/app/subscription.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "lockstep/domain/events.hpp"

namespace lockstep::app {

namespace {

enum class EventCategory : std::uint8_t { Trade, BookUpdate, Private };

EventCategory category_of(const domain::Event& event) {
    return std::visit(
        []<typename T>(const T&) {
            if constexpr (std::is_same_v<T, domain::Trade>) {
                return EventCategory::Trade;
            } else if constexpr (std::is_same_v<T, domain::BookLevelChanged> ||
                                 std::is_same_v<T, domain::InstrumentStatusChanged>) {
                // InstrumentStatusChanged (halt/resume) is public market-
                // structure data like a book update, not one of the
                // order-lifecycle/risk events SubscriptionFilter::private_events
                // lists, so it travels on the same flag as book updates.
                return EventCategory::BookUpdate;
            } else {
                // OrderAccepted/OrderCancelled/OrderModified/RiskCommandApplied:
                // exactly the private_events list in subscription.hpp's doc.
                return EventCategory::Private;
            }
        },
        event);
}

std::optional<domain::InstrumentId> instrument_of(const domain::Event& event) {
    return std::visit(
        []<typename T>(const T& payload) -> std::optional<domain::InstrumentId> {
            if constexpr (requires { payload.instrument; }) {
                return payload.instrument;
            } else {
                // RiskCommandApplied carries no instrument: it is a per-shard
                // broadcast ack, not scoped to one book, so an instrument
                // filter never excludes it.
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
    // shared_ptr to it (see the class comment), so on_ready() cannot race
    // this load.
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
    return overflowed_.load(std::memory_order_relaxed);  // relaxed: see deliver()
}

void Subscription::cancel() noexcept {
    // relaxed: a pure signal with no accompanying data to order - the ring's
    // own SpscQueue acquire/release pairs already order its contents
    // independently of this flag, and cancellation does not need to be seen
    // atomically with anything else.
    cancelled_.store(true, std::memory_order_relaxed);
}

bool Subscription::cancelled() const noexcept {
    return cancelled_.load(std::memory_order_relaxed);  // relaxed: see cancel()
}

void Subscription::on_ready(std::move_only_function<void() noexcept> callback) {
    auto* owned = new std::move_only_function<void() noexcept>(std::move(callback));
    // release: pairs with the acquire load in notify_ready(), publishing the
    // callback's captured state (e.g. a reactor pointer) before the
    // publisher thread can invoke it.
    std::move_only_function<void() noexcept>* previous =
        on_ready_.exchange(owned, std::memory_order_release);
    delete previous;  // on_ready() is documented to be set once, before the
                      // first poll(); this just avoids a leak if a caller
                      // replaces it anyway.
}

bool Subscription::wants(const PublishedEvent& event) const {
    if (!filter_.instruments.empty()) {
        const std::optional<domain::InstrumentId> instrument = instrument_of(event.event);
        if (instrument.has_value() &&
            std::ranges::find(filter_.instruments, *instrument) == filter_.instruments.end()) {
            return false;
        }
    }
    switch (category_of(event.event)) {
        case EventCategory::Trade:
            return filter_.trades;
        case EventCategory::BookUpdate:
            return filter_.book_updates;
        case EventCategory::Private:
            return filter_.private_events;
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
    // relaxed: see cancel()'s comment; whatever is already in the ring stays
    // independently ordered by SpscQueue's own acquire/release pairing.
    overflowed_.store(true, std::memory_order_relaxed);
    return false;
}

void Subscription::notify_ready() noexcept {
    // acquire: pairs with the release in on_ready(); see its comment.
    auto* callback = on_ready_.load(std::memory_order_acquire);
    if (callback != nullptr) {
        (*callback)();
    }
}

}  // namespace lockstep::app
