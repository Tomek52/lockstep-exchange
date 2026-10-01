// Task 011: runtime subscriptions and the slow-consumer policy.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <span>
#include <stop_token>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/app/subscription.hpp"

#include "app/test_support.hpp"

namespace lockstep::app {
namespace {

using namespace domain;

NewOrder buy(InstrumentId instrument) {
    return NewOrder{.trader = TraderId{1},
                    .client_order_id = ClientOrderId{1},
                    .instrument = instrument,
                    .side = Side::Buy,
                    .type = OrderType::Limit,
                    .time_in_force = TimeInForce::Gtc,
                    .price = Price{100},
                    .quantity = Quantity{10}};
}

// Hand-fed OutboundItem payloads for the Publisher-level regression tests
// below, which construct egress items directly (no shard involved) - every
// field filled explicitly, since only `instrument` and `sequence` matter to
// those tests.
OrderAccepted accepted_event(InstrumentId instrument) {
    return OrderAccepted{.order_id = OrderId{1},
                         .trader = TraderId{1},
                         .client_order_id = ClientOrderId{1},
                         .instrument = instrument,
                         .side = Side::Buy,
                         .type = OrderType::Limit,
                         .price = Price{100},
                         .quantity = Quantity{10}};
}

BookLevelChanged level_changed_event(InstrumentId instrument) {
    return BookLevelChanged{
        .instrument = instrument, .side = Side::Buy, .price = Price{100}, .quantity = Quantity{10}};
}

// Bounded poll instead of a raw wait: a failure reports instead of hanging
// the test binary (CLAUDE.md/task guidance for every regression here).
template <typename Pred>
bool wait_until(Pred predicate, std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

class PublisherSubscriptionTest : public ::testing::Test {
protected:
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    // One shard for both instruments: keeps event order across the two
    // commands in these tests unambiguous (ADR-0003 only guarantees order
    // per shard).
    Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}, {.id = InstrumentId{2}}},
                               .shard_count = 1},
                  journals.factory(), clock};

    void SetUp() override { engine.start(); }

    void submit_and_wait(domain::Command command) {
        auto [completion, reply] = test::reply_future();
        ASSERT_TRUE(engine.submit(command, std::move(completion)).has_value());
        ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    }

    // Retries on a transiently full ingress instead of asserting - used by
    // tests that fire commands faster than one shard thread drains.
    void submit_async(domain::Command command) {
        while (!engine.submit(command, [](const CommandReply&) noexcept {}).has_value()) {
            std::this_thread::yield();
        }
    }
};

// --- Acceptance criterion 1: instrument filtering, shard sequence order ---

TEST_F(PublisherSubscriptionTest, FilteredSubscriptionSeesOnlyThatInstrumentInOrder) {
    // private_events=true: this test is about the instrument filter, not the
    // event-category filter, so it also wants OrderAccepted (a private event
    // per SubscriptionFilter's doc) alongside the public BookLevelChanged.
    auto sub = engine.subscribe(
        SubscriptionFilter{.instruments = {InstrumentId{2}}, .private_events = true}, 64);

    submit_and_wait(buy(InstrumentId{1}));  // instrument 1: must never appear below

    // Several instrument-2 commands, each with a distinct client_order_id
    // (dedup is keyed on (trader, client_order_id) across every book in the
    // shard, not per instrument, and the first order of each pair is still
    // resting).
    for (std::uint64_t i = 2; i <= 5; ++i) {
        NewOrder order = buy(InstrumentId{2});
        order.client_order_id = ClientOrderId{i};
        submit_and_wait(order);
    }

    std::vector<PublishedEvent> buf(32);
    const std::size_t n = sub->poll(std::span{buf});
    // 4 commands x (OrderAccepted, BookLevelChanged) - non-crossing GTC
    // limits, nothing from instrument 1's command.
    ASSERT_EQ(n, 8U);
    SequenceNumber previous{0};
    for (std::size_t i = 0; i < n; i += 2) {
        ASSERT_TRUE(std::holds_alternative<OrderAccepted>(buf[i].event));
        EXPECT_EQ(std::get<OrderAccepted>(buf[i].event).instrument, InstrumentId{2});
        ASSERT_TRUE(std::holds_alternative<BookLevelChanged>(buf[i + 1].event));
        EXPECT_EQ(std::get<BookLevelChanged>(buf[i + 1].event).instrument, InstrumentId{2});
        // Both events of one command share its sequence, and shard sequence
        // numbers are strictly increasing per command.
        EXPECT_EQ(buf[i].sequence, buf[i + 1].sequence);
        EXPECT_GT(buf[i].sequence, previous);
        previous = buf[i].sequence;
    }
    EXPECT_FALSE(sub->overflowed());
}

// --- Acceptance criterion 2: slow consumer never blocks the publisher ---

TEST_F(PublisherSubscriptionTest, SlowConsumerDoesNotBlockPublisherOrFastSubscription) {
    test::RecordingSubscriber all;  // the ground truth: every event, unfiltered
    engine.add_subscriber(all);

    auto slow = engine.subscribe(SubscriptionFilter{}, 8);
    // Large enough that ~10,000 commands' worth of OrderAccepted/Trade/
    // BookLevelChanged events (at most a handful per command) cannot
    // overflow it - this subscription is the control for "the publisher
    // keeps delivering to a fast subscription" while `slow` is never polled.
    auto fast = engine.subscribe(SubscriptionFilter{}, 1U << 17U);

    // Count calls, and specifically whether overflowed() was ever observed
    // true from inside the callback: on_ready fires once per flush that
    // delivers anything to this subscription (its documented contract, not
    // just on overflow), so with capacity 8 and a steady stream it legitimately
    // fires several times before the ring finally fills - what the overflow
    // policy actually promises is that overflow is among the things it gets
    // told about, not that it is the only thing.
    std::atomic<int> slow_ready_calls{0};
    std::atomic<bool> overflow_seen_in_callback{false};
    ASSERT_TRUE(slow->on_ready([&]() noexcept {
        if (slow->overflowed()) {
            overflow_seen_in_callback.store(true, std::memory_order_relaxed);
        }
        slow_ready_calls.fetch_add(1, std::memory_order_relaxed);
    }));

    constexpr int total = 10'000;
    std::atomic<int> accepted{0};
    for (int i = 0; i < total; ++i) {
        NewOrder order = buy(InstrumentId{1});
        order.client_order_id = ClientOrderId{static_cast<std::uint64_t>(i)};
        order.side = (i % 2 == 0) ? Side::Buy : Side::Sell;  // crosses, generates Trade events too
        // Retry on a transiently full ingress (this loop submits far faster
        // than one shard thread drains) - the thing under test is the
        // publisher/subscription path, not ingress sizing.
        while (true) {
            auto submitted = engine.submit(order, [&accepted](const CommandReply&) noexcept {
                accepted.fetch_add(1, std::memory_order_relaxed);
            });
            if (submitted.has_value()) {
                break;
            }
            ASSERT_EQ(submitted.error(), SubmitError::Overloaded);
            std::this_thread::yield();
        }
    }

    // Bounded wait, not a hang: if a full, never-polled `slow` ever blocked
    // the publisher (and transitively the shard, ADR-0003), accepted would
    // stall below total and this loop would run out the clock instead of
    // completing promptly.
    EXPECT_TRUE(wait_until([&] { return accepted.load(std::memory_order_relaxed) == total; },
                           std::chrono::seconds{10}))
        << "a full, never-polled subscription stalled the publisher";

    EXPECT_TRUE(slow->overflowed());
    EXPECT_GE(slow_ready_calls.load(std::memory_order_relaxed), 1) << "on_ready never fired";
    EXPECT_TRUE(overflow_seen_in_callback.load(std::memory_order_relaxed))
        << "on_ready never fired while overflowed() was observably true";

    // Drained (and checked) before engine.stop(): Publisher::run's shutdown
    // closes every still-registered subscription (same observable effect as
    // overflow, see Subscription::close()'s doc) so a consumer blocked only
    // on on_ready() cannot hang past shutdown - that would make `fast`
    // legitimately read overflowed()==true too, for an unrelated reason,
    // after stop() runs.
    std::vector<PublishedEvent> buf(256);
    std::size_t drained = 0;
    std::size_t n = 0;
    while ((n = fast->poll(std::span{buf})) > 0) {
        drained += n;
    }
    EXPECT_FALSE(fast->overflowed());
    // trades + book_updates only (fast's default filter excludes the
    // private OrderAccepted); `all` is unfiltered, so count only the
    // categories fast actually wants.
    std::size_t expected = 0;
    for (const PublishedEvent& event : all.events()) {
        if (!std::holds_alternative<OrderAccepted>(event.event)) {
            ++expected;
        }
    }
    EXPECT_EQ(drained, expected);

    engine.stop();
}

// --- Acceptance criterion 4: mid-stream registration, no partial command ---

TEST_F(PublisherSubscriptionTest, SubscriptionMidStreamSeesOnlyLaterEventsAtomically) {
    submit_and_wait(buy(InstrumentId{1}));

    // private_events=true: see FilteredSubscriptionSeesOnlyThatInstrumentInOrder.
    auto sub = engine.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 64);

    // Distinct client_order_id: the first order is still resting and dedup
    // is keyed on (trader, client_order_id) across every book in the shard.
    NewOrder second = buy(InstrumentId{2});
    second.client_order_id = ClientOrderId{2};
    submit_and_wait(second);

    std::vector<PublishedEvent> buf(16);
    const std::size_t n = sub->poll(std::span{buf});
    // Only the second command's events (OrderAccepted + BookLevelChanged);
    // the first command's pair, produced before registration, must not
    // appear even partially.
    ASSERT_EQ(n, 2U);
    ASSERT_TRUE(std::holds_alternative<OrderAccepted>(buf[0].event));
    EXPECT_EQ(std::get<OrderAccepted>(buf[0].event).instrument, InstrumentId{2});
    ASSERT_TRUE(std::holds_alternative<BookLevelChanged>(buf[1].event));
    EXPECT_EQ(std::get<BookLevelChanged>(buf[1].event).instrument, InstrumentId{2});
}

TEST_F(PublisherSubscriptionTest, CancelStopsDelivery) {
    auto sub = engine.subscribe(SubscriptionFilter{}, 64);
    sub->cancel();
    sub->cancel();  // idempotent

    submit_and_wait(buy(InstrumentId{1}));
    engine.stop();

    std::vector<PublishedEvent> buf(16);
    EXPECT_EQ(sub->poll(std::span{buf}), 0U);
    EXPECT_FALSE(sub->overflowed());
}

// --- Acceptance criterion 3: concurrent subscribe/cancel under traffic ---

TEST_F(PublisherSubscriptionTest, ConcurrentSubscribeAndCancelFromFourThreadsIsRaceFree) {
    std::atomic<bool> stop_workers{false};
    std::vector<std::jthread> workers;
    workers.reserve(4);
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([this, &stop_workers, worker] {
            while (!stop_workers.load(std::memory_order_relaxed)) {
                auto sub =
                    engine.subscribe(SubscriptionFilter{.instruments = {InstrumentId{1}}}, 4);
                // Owned, heap-allocated captured state + cancel() + destroy
                // right after: the pattern a gRPC reactor's OnDone follows
                // (task 013), and the one cancel()'s handshake has to make
                // safe (concurrency review R3) - this is a TSan target, not
                // an assertion on content.
                auto* captured = new int{worker};
                ASSERT_TRUE(sub->on_ready([captured]() noexcept { (void)*captured; }));
                std::vector<PublishedEvent> buf(4);
                (void)sub->poll(std::span{buf});
                sub->cancel();
                delete captured;
            }
        });
    }

    // Distinct, crossing orders so the shard actually produces a steady
    // stream of Trade/BookLevelChanged events for the subscribers above to
    // race against, instead of every order after the first being rejected
    // as a duplicate client_order_id (both instruments share a trader, and
    // the first resting order would otherwise block every later one).
    constexpr int total = 1'000;
    for (int i = 0; i < total; ++i) {
        NewOrder order = buy((i % 2) == 0 ? InstrumentId{1} : InstrumentId{2});
        order.client_order_id = ClientOrderId{static_cast<std::uint64_t>(i)};
        order.side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        submit_async(order);
    }

    stop_workers.store(true, std::memory_order_relaxed);
    workers.clear();  // joins every worker
    engine.stop();
}

// --- Regression: many subscriptions registered during a flood of traffic
// must never see a partial command (concurrency review R4). ---

TEST_F(PublisherSubscriptionTest,
       ManySubscriptionsDuringHighThroughputTrafficNeverSeePartialCommand) {
    test::RecordingSubscriber all;
    engine.add_subscriber(all);

    std::atomic<bool> traffic_done{false};
    std::jthread traffic{[&] {
        for (std::uint64_t i = 1; i <= 20'000; ++i) {
            NewOrder order = buy(InstrumentId{1});
            order.client_order_id = ClientOrderId{i};
            order.side = (i % 2) != 0 ? Side::Buy : Side::Sell;
            submit_async(order);
        }
        traffic_done.store(true, std::memory_order_relaxed);
    }};

    std::vector<std::shared_ptr<Subscription>> subs;
    while (!traffic_done.load(std::memory_order_relaxed) && subs.size() < 400) {
        subs.push_back(engine.subscribe(
            SubscriptionFilter{.instruments = {}, .private_events = true}, 1U << 16U));
        std::this_thread::sleep_for(std::chrono::microseconds{50});
    }
    traffic.join();
    engine.stop();

    std::map<std::uint64_t, std::size_t> events_per_sequence;
    for (const PublishedEvent& event : all.events()) {
        ++events_per_sequence[event.sequence.value()];
    }

    std::vector<PublishedEvent> buf(1U << 16U);
    int partial = 0;
    for (const auto& sub : subs) {
        const std::size_t n = sub->poll(std::span{buf});
        if (n == 0) {
            continue;
        }
        const std::uint64_t first_sequence = buf[0].sequence.value();
        std::size_t got = 0;
        for (std::size_t k = 0; k < n && buf[k].sequence.value() == first_sequence; ++k) {
            ++got;
        }
        if (got != events_per_sequence[first_sequence]) {
            ++partial;
            if (partial <= 5) {
                ADD_FAILURE() << "subscription's first command (seq " << first_sequence << ") got "
                              << got << " of " << events_per_sequence[first_sequence] << " events";
            }
        }
    }
    EXPECT_EQ(partial, 0) << "of " << subs.size() << " subscriptions";
}

// --- Regression: a subscription registered between two try_push() calls of
// the SAME command must receive neither the already-popped prefix nor the
// not-yet-popped suffix - deterministic, Publisher-level (hand-fed
// EgressQueue, no Engine/shard involved), per the task 011 code and
// concurrency reviews. This reproduces the bug `release_staged()` can
// trigger: it pushes one command's events to egress one try_push() at a
// time, so the publisher can observe "empty" between two events of the
// SAME command. ---

TEST(PublisherPartialCommandRegression,
     SubscriptionRegisteredBetweenTwoPushesOfOneCommandGetsNone) {
    EgressQueue egress{64};
    concurrency::Doorbell bell;
    Publisher publisher{{&egress}, bell};
    // A witness subscribed before anything happens, to pace this test off
    // (poll for what it has already seen) without an arbitrary sleep.
    auto witness =
        publisher.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 64);
    std::jthread publisher_thread{[&](const std::stop_token& stop) { publisher.run(stop); }};

    auto event = [](std::uint64_t sequence, Event payload) {
        return PublishedEvent{ShardId{0}, SequenceNumber{sequence}, Timestamp{1}, payload};
    };

    // Command 7's first event only - as if release_staged() were preempted
    // between pushing it and the rest of command 7.
    ASSERT_TRUE(egress.try_push(OutboundItem{event(7, accepted_event(InstrumentId{1}))}));
    bell.ring();
    std::vector<PublishedEvent> buf(16);
    ASSERT_TRUE(wait_until([&] { return witness->poll(std::span{buf}) == 1U; }));

    // A new client subscribes in that exact gap.
    auto late =
        publisher.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 64);

    // The rest of command 7, then all of command 8.
    ASSERT_TRUE(egress.try_push(OutboundItem{event(7, level_changed_event(InstrumentId{1}))}));
    ASSERT_TRUE(egress.try_push(OutboundItem{event(8, level_changed_event(InstrumentId{1}))}));
    bell.ring();

    std::vector<PublishedEvent> got;
    ASSERT_TRUE(wait_until([&] {
        const std::size_t n = late->poll(std::span{buf});
        got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
        return got.size() >= 1 && got.front().sequence == SequenceNumber{8};
    }));

    publisher_thread.request_stop();
    publisher_thread.join();

    // Must never have received any part of command 7 - not the prefix (it
    // was already popped before registration) and not the suffix either.
    for (const PublishedEvent& received : got) {
        EXPECT_NE(received.sequence, SequenceNumber{7})
            << "received part of a command that started before registration";
    }
}

// --- Regression: a command submitted strictly after subscribe() returns
// must always be delivered in full (the CI failure this task's PR hit:
// draining the control queue only once per poll_once() call left a window
// where a long inner loop could pop a brand-new command's events before
// ever re-checking it). Publisher-level, hand-fed, so the registration and
// the second command's egress pushes can be sequenced deterministically
// within what would be a single poll_once() iteration. ---

// This is the shape of the CI failure itself: draining the control queue
// only once per poll_once() call (instead of once per pop - see the
// Publisher class comment) leaves a window where a single call's inner loop
// pops many commands - including a brand-new one - without ever re-checking
// it. Reproducing that deterministically needs the inner loop to actually
// be mid-flight (not idle, not already returned) at the moment subscribe()
// runs, so this pre-loads a large run of filler commands - all already
// sitting in egress, nothing to force poll_once() to return early - and
// races a concurrent subscribe() against the single long poll_once() call
// draining them. With the once-per-call placement this is flaky in the
// wrong direction (sometimes wins the race, sometimes loses it); with the
// once-per-pop placement it cannot lose. 200 iterations under debug, release
// and tsan (see the task's definition-of-done run) is the evidence for that.
TEST(PublisherPartialCommandRegression, CommandSubmittedAfterSubscribeReturnsIsDeliveredInFull) {
    constexpr std::uint64_t filler_commands = 4'000;
    EgressQueue egress{1U << 16U};
    concurrency::Doorbell bell;
    Publisher publisher{{&egress}, bell, /*max_batch=*/1U << 20U};

    auto event = [](std::uint64_t sequence, Event payload) {
        return PublishedEvent{ShardId{0}, SequenceNumber{sequence}, Timestamp{1}, payload};
    };

    // All filler commands' events already sitting in egress before the
    // publisher thread even starts, so its very first poll_once() call's
    // inner loop has a long, uninterrupted run of items to pop - no gap
    // where it would return and re-drain the control queue on its own.
    // Instrument 2, filtered out below, regardless of exactly when the
    // subscription ends up registering relative to this run - only the
    // target command (instrument 1) should ever reach it.
    for (std::uint64_t seq = 1; seq <= filler_commands; ++seq) {
        ASSERT_TRUE(
            egress.try_push(OutboundItem{event(seq, level_changed_event(InstrumentId{2}))}));
    }
    // The command this test is actually about, appended right after the
    // filler - same single contiguous run, nothing to force an intervening
    // flush in between it and the filler.
    const std::uint64_t target_sequence = filler_commands + 1;
    ASSERT_TRUE(
        egress.try_push(OutboundItem{event(target_sequence, accepted_event(InstrumentId{1}))}));
    ASSERT_TRUE(egress.try_push(
        OutboundItem{event(target_sequence, level_changed_event(InstrumentId{1}))}));

    std::shared_ptr<Subscription> sub;
    std::jthread subscriber{[&] {
        // No synchronization with ring()/run() below on purpose: the race
        // this test needs is "subscribe() lands while poll_once()'s inner
        // loop is already mid-flight", and thread start-up latency alone
        // reliably puts it somewhere in that multi-thousand-item run.
        sub = publisher.subscribe(
            SubscriptionFilter{.instruments = {InstrumentId{1}}, .private_events = true}, 64);
    }};
    std::jthread publisher_thread{[&](const std::stop_token& stop) { publisher.run(stop); }};
    bell.ring();
    subscriber.join();

    std::vector<PublishedEvent> buf(16);
    std::vector<PublishedEvent> got;
    ASSERT_TRUE(wait_until(
        [&] {
            const std::size_t n = sub->poll(std::span{buf});
            got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
            return got.size() >= 2;
        },
        std::chrono::seconds{10}));

    publisher_thread.request_stop();
    publisher_thread.join();

    ASSERT_EQ(got.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(got[0].event));
    EXPECT_EQ(got[0].sequence, SequenceNumber{target_sequence});
    EXPECT_TRUE(std::holds_alternative<BookLevelChanged>(got[1].event));
    EXPECT_EQ(got[1].sequence, SequenceNumber{target_sequence});
}

// --- Regression (concurrency review R1/R1b): on_ready() set after events
// have already been delivered does not retroactively fire for them - this
// is Subscription::on_ready's documented contract ("Set before first
// poll()"), not a bug: a consumer that installs the hook late must still
// poll once on its own to pick up anything already waiting. ---

TEST_F(PublisherSubscriptionTest, OnReadySetAfterDeliveryDoesNotFireButDataIsStillThere) {
    auto sub = engine.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 64);
    submit_and_wait(buy(InstrumentId{1}));  // delivered before on_ready() below

    std::atomic<int> fired{0};
    ASSERT_TRUE(
        sub->on_ready([&fired]() noexcept { fired.fetch_add(1, std::memory_order_relaxed); }));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Checked before engine.stop(): Publisher::run's shutdown closes every
    // still-registered subscription and fires on_ready once as part of
    // that (Subscription::close()'s doc) - a second, legitimate reason for
    // the hook to fire that is not what this test is about.
    EXPECT_EQ(fired.load(std::memory_order_relaxed), 0)
        << "on_ready is documented to not re-fire for events delivered before it was set";
    std::vector<PublishedEvent> buf(16);
    EXPECT_GT(sub->poll(std::span{buf}), 0U) << "the data must still be retrievable by polling";

    engine.stop();
}

TEST_F(PublisherSubscriptionTest, OverflowBeforeOnReadyIsNotRetroactivelySignalled) {
    auto sub = engine.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 2);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        NewOrder order = buy(InstrumentId{1});
        order.client_order_id = ClientOrderId{i};
        submit_and_wait(order);
    }
    ASSERT_TRUE(sub->overflowed());

    std::atomic<int> fired{0};
    ASSERT_TRUE(
        sub->on_ready([&fired]() noexcept { fired.fetch_add(1, std::memory_order_relaxed); }));
    for (std::uint64_t i = 5; i <= 8; ++i) {
        NewOrder order = buy(InstrumentId{1});
        order.client_order_id = ClientOrderId{i};
        submit_and_wait(order);
    }
    engine.stop();

    // Already overflowed before on_ready() was installed, and deliver_to()
    // skips an already-overflowed subscription entirely (no further
    // delivery attempts, so no further notify_ready() calls either) - the
    // consumer must check overflowed() itself rather than rely on a fresh
    // hook to be told about something that already happened.
    EXPECT_EQ(fired.load(std::memory_order_relaxed), 0)
        << "on_ready is documented to not re-fire for an overflow that preceded it";
}

// --- Regression (concurrency review R2): a second on_ready() call must be
// rejected, never replace a callback the publisher might be running. ---

TEST_F(PublisherSubscriptionTest, SecondOnReadyCallIsRejected) {
    auto sub = engine.subscribe(SubscriptionFilter{}, 64);
    ASSERT_TRUE(sub->on_ready([]() noexcept {}));
    EXPECT_FALSE(sub->on_ready([]() noexcept {}));
}

TEST_F(PublisherSubscriptionTest, ReplacingOnReadyUnderTrafficNeverRacesNotify) {
    auto sub =
        engine.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 1U << 20U);
    ASSERT_TRUE(sub->on_ready([]() noexcept {}));

    std::atomic<bool> traffic_done{false};
    std::jthread traffic{[&] {
        for (std::uint64_t i = 1; i <= 3'000; ++i) {
            NewOrder order = buy(InstrumentId{1});
            order.client_order_id = ClientOrderId{i};
            order.side = (i % 2) != 0 ? Side::Buy : Side::Sell;
            submit_async(order);
        }
        traffic_done.store(true, std::memory_order_relaxed);
    }};

    std::vector<PublishedEvent> buf(256);
    while (!traffic_done.load(std::memory_order_relaxed)) {
        // Every one of these must be rejected (false): the first on_ready()
        // above already won. Rejection, not replacement, is what makes this
        // race-free under TSan (a successful replace would be freeing a
        // callback the publisher thread might currently be inside).
        EXPECT_FALSE(sub->on_ready([]() noexcept {}));
        (void)sub->poll(std::span{buf});
    }
    traffic.join();
    engine.stop();
}

// --- Regression (concurrency review R3): cancel() must guarantee the
// on_ready hook never runs again once it returns, so the consumer can
// safely destroy whatever it captured right after (a gRPC reactor's
// OnDone). TSan target. ---

TEST_F(PublisherSubscriptionTest, CancelThenDestroyCapturedStateNeverRacesNotify) {
    std::atomic<bool> traffic_done{false};
    std::jthread traffic{[&] {
        for (std::uint64_t i = 1; i <= 4'000; ++i) {
            NewOrder order = buy(InstrumentId{1});
            order.client_order_id = ClientOrderId{i};
            order.side = (i % 2) != 0 ? Side::Buy : Side::Sell;
            submit_async(order);
        }
        traffic_done.store(true, std::memory_order_relaxed);
    }};

    std::vector<std::jthread> consumers;
    consumers.reserve(4);
    for (int worker = 0; worker < 4; ++worker) {
        consumers.emplace_back([&] {
            std::vector<PublishedEvent> buf(64);
            while (!traffic_done.load(std::memory_order_relaxed)) {
                auto* captured = new std::uint64_t{0};
                auto sub = engine.subscribe(
                    SubscriptionFilter{.instruments = {}, .private_events = true}, 1024);
                ASSERT_TRUE(sub->on_ready([captured]() noexcept { ++*captured; }));
                (void)sub->poll(std::span{buf});
                std::this_thread::yield();
                sub->cancel();  // must return only once no on_ready() call can still be running
                delete captured;
            }
        });
    }

    traffic.join();
    consumers.clear();
    engine.stop();
}

// --- Regression (concurrency review R5 / code review #3): subscribe()
// after the publisher has stopped must not spin forever once the control
// queue fills. ---

TEST_F(PublisherSubscriptionTest, SubscribeAfterStopReturnsClosedInsteadOfHanging) {
    engine.stop();

    std::atomic<int> completed{0};
    std::jthread subscriber{[&] {
        for (int i = 0; i < 1100; ++i) {  // past the control queue's 1024 capacity
            (void)engine.subscribe({}, 2);
            completed.fetch_add(1, std::memory_order_relaxed);
        }
    }};

    EXPECT_TRUE(wait_until([&] { return completed.load(std::memory_order_relaxed) == 1100; },
                           std::chrono::seconds{5}))
        << "subscribe() after stop() is stuck after " << completed.load(std::memory_order_relaxed)
        << " calls";
    subscriber.join();

    // And the returned subscription is usably "closed", not just non-hanging.
    auto sub = engine.subscribe({}, 2);
    EXPECT_TRUE(sub->overflowed());
    std::vector<PublishedEvent> buf(4);
    EXPECT_EQ(sub->poll(std::span{buf}), 0U);
}

}  // namespace
}  // namespace lockstep::app
