// Task 011: runtime subscriptions and the slow-consumer policy.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <mutex>
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

// Runs `body` on a helper thread and reports whether it returned within
// `limit`, instead of letting a regression hang the whole test binary: a
// hang regression needs a bounded wait, not a raw blocking call, to be
// reported as a failure rather than wedging CI. A body that never returns
// leaks its thread (detach()) - acceptable for a handful of
// process-lifetime test failures, never for passing runs.
template <typename F>
bool completes_within(F body, std::chrono::milliseconds limit = std::chrono::seconds{5}) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread worker{[done, body = std::move(body)]() mutable {
        body();
        done->store(true, std::memory_order_release);
    }};
    const bool finished = wait_until([&] { return done->load(std::memory_order_acquire); }, limit);
    if (finished) {
        worker.join();
    } else {
        worker.detach();
    }
    return finished;
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
    // Every event any worker below ever polled, so this test also checks
    // some content, not just "no TSan report". The complementary "never a
    // partial command" invariant is checked by
    // ManySubscriptionsDuringHighThroughputTrafficNeverSeePartialCommand,
    // under conditions that do not have this test's own confound: a
    // subscription here is cancelled moments after its one poll(), so a
    // shard that is itself preempted mid-release_staged() (ShardRuntime;
    // unrelated to subscriptions, and already covered by task 007's own
    // tests) can legitimately make what THIS short-lived subscription sees
    // before it cancels incomplete, without that being a registration bug.
    std::mutex collected_mutex;
    std::vector<PublishedEvent> collected;

    std::atomic<bool> stop_workers{false};
    std::vector<std::jthread> workers;
    workers.reserve(4);
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([this, &stop_workers, &collected_mutex, &collected, worker] {
            while (!stop_workers.load(std::memory_order_relaxed)) {
                auto sub =
                    engine.subscribe(SubscriptionFilter{.instruments = {InstrumentId{1}}}, 16);
                // Owned, heap-allocated captured state + cancel() + destroy
                // right after: the pattern a gRPC reactor's OnDone follows
                // (task 013), and the one cancel()'s handshake has to make
                // safe (concurrency review R3).
                auto* captured = new int{worker};
                ASSERT_TRUE(sub->on_ready([captured]() noexcept { (void)*captured; }));
                // Brief bounded retry, not one immediate poll(): registering
                // and polling back to back otherwise has every chance of
                // landing in the gap between two flushes rather than during
                // one, starving this check of any data to look at even
                // though traffic is flowing continuously elsewhere.
                std::vector<PublishedEvent> buf(16);
                std::size_t n = 0;
                for (int attempt = 0; attempt < 200 && n == 0; ++attempt) {
                    n = sub->poll(std::span{buf});
                    if (n == 0) {
                        std::this_thread::yield();
                    }
                }
                if (n > 0) {
                    const std::lock_guard<std::mutex> lock(collected_mutex);
                    collected.insert(collected.end(), buf.begin(),
                                     buf.begin() + static_cast<std::ptrdiff_t>(n));
                }
                sub->cancel();
                delete captured;
            }
        });
    }

    // Distinct orders that actually cross within each instrument's own
    // book (side alternates every *other* order on the SAME instrument,
    // not every order overall - i%2 for both instrument and side meant
    // instrument 1 only ever saw Buy and instrument 2 only ever saw Sell,
    // so nothing ever crossed and the subscribers above raced against
    // nothing but idle resting orders - see the task's concurrency
    // review), so the shard actually produces a steady stream of
    // Trade/BookLevelChanged events for the subscribers above to race
    // against.
    constexpr int total = 20'000;
    for (int i = 0; i < total; ++i) {
        const InstrumentId instrument = (i % 2) == 0 ? InstrumentId{1} : InstrumentId{2};
        NewOrder order = buy(instrument);
        order.client_order_id = ClientOrderId{static_cast<std::uint64_t>(i)};
        order.side = ((i / 2) % 2 == 0) ? Side::Buy : Side::Sell;
        submit_async(order);
    }

    stop_workers.store(true, std::memory_order_relaxed);
    workers.clear();  // joins every worker
    engine.stop();

    ASSERT_GT(collected.size(), 0U) << "no worker ever received any event - the orders above "
                                       "still are not actually crossing";
    for (const PublishedEvent& event : collected) {
        ASSERT_TRUE(std::holds_alternative<Trade>(event.event) ||
                    std::holds_alternative<BookLevelChanged>(event.event));
        const InstrumentId instrument = std::holds_alternative<Trade>(event.event)
                                            ? std::get<Trade>(event.event).instrument
                                            : std::get<BookLevelChanged>(event.event).instrument;
        EXPECT_EQ(instrument, InstrumentId{1});
    }
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
    // The publisher thread must already be running: subscribe() blocks
    // until the publisher itself drains and acknowledges the registration
    // (Publisher::subscribe's doc), so calling it before anything can ever
    // drain control_ would deadlock this (the calling) thread.
    std::jthread publisher_thread{[&](const std::stop_token& stop) { publisher.run(stop); }};
    // A witness subscribed before anything happens, to pace this test off
    // (poll for what it has already seen) without an arbitrary sleep.
    auto witness =
        publisher.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 64);

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

// --- Regression: a command pushed strictly after subscribe() returns must
// always be delivered in full (Publisher class comment's corollary).
// Deterministic in the sense that program order guarantees "after
// subscribe() returns" - the push only happens once subscribe() has already
// returned - but the *miss* this reproduces needs concurrent noise: a
// witness subscribed before the first production-code fix shows this is
// not a timing coincidence of one specific interleaving, it is
// control_'s (a multi-producer queue) stall property making an
// already-returned subscription's own, already-landed push temporarily
// unreachable to drain_control() while a concurrently-stalled *other*
// producer sits earlier in the queue - which only concurrent subscribe()
// traffic can actually create. Without the noise threads below this
// passed even on the version that only fixed the preemption window, not
// the queue-stall one; the noise is what makes it fail against that
// version and pass against this one. ---
TEST(PublisherPartialCommandRegression, CommandPushedAfterSubscribeReturnsIsDeliveredInFull) {
    constexpr std::uint64_t iterations = 3'000;
    constexpr int noise_threads = 3;
    EgressQueue egress{1U << 16U};
    concurrency::Doorbell bell;
    Publisher publisher{{&egress}, bell};
    std::jthread publisher_thread{[&](const std::stop_token& stop) { publisher.run(stop); }};

    // Concurrent subscribe()/cancel() traffic on an unrelated instrument,
    // purely to create the control_ stall window above - it must never
    // itself receive anything, so it cannot be mistaken for the signal
    // under test.
    std::atomic<bool> stop_noise{false};
    std::vector<std::jthread> noise;
    noise.reserve(noise_threads);
    for (int t = 0; t < noise_threads; ++t) {
        noise.emplace_back([&] {
            while (!stop_noise.load(std::memory_order_relaxed)) {
                auto sub =
                    publisher.subscribe(SubscriptionFilter{.instruments = {InstrumentId{9}}}, 2);
                sub->cancel();
            }
        });
    }

    auto event1 = [](std::uint64_t sequence) {
        return PublishedEvent{ShardId{0}, SequenceNumber{sequence}, Timestamp{1},
                              accepted_event(InstrumentId{1})};
    };
    auto event2 = [](std::uint64_t sequence) {
        return PublishedEvent{ShardId{0}, SequenceNumber{sequence}, Timestamp{1},
                              level_changed_event(InstrumentId{1})};
    };

    int missed = 0;
    std::vector<PublishedEvent> buf(8);
    for (std::uint64_t k = 1; k <= iterations; ++k) {
        auto sub =
            publisher.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 8);
        // subscribe() is documented to block until registration has
        // actually taken effect - check that directly, not just infer it
        // from delivery below (code review item 4).
        EXPECT_TRUE(sub->is_registered());
        // subscribe() has returned: everything below is "after registration".
        // Two events for this one command, same as a real OrderAccepted +
        // BookLevelChanged pair: a fix that only kept the FIRST event of a
        // sequence from being dropped (rather than the whole command) would
        // not be caught by asserting on just one.
        while (!egress.try_push(OutboundItem{event1(k)})) {
            bell.ring();
            std::this_thread::yield();
        }
        while (!egress.try_push(OutboundItem{event2(k)})) {
            bell.ring();
            std::this_thread::yield();
        }
        bell.ring();

        bool got_first = false;
        bool got_second = false;
        // Widened from 200ms: under the background CPU load this suite is
        // stressed with (definition-of-done run), noise_threads alone can
        // starve this poller for longer than that without anything actually
        // being lost.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
        while (!(got_first && got_second) && std::chrono::steady_clock::now() < deadline) {
            const std::size_t n = sub->poll(std::span{buf});
            for (std::size_t i = 0; i < n; ++i) {
                if (buf[i].sequence != SequenceNumber{k}) {
                    continue;
                }
                got_first |= std::holds_alternative<OrderAccepted>(buf[i].event);
                got_second |= std::holds_alternative<BookLevelChanged>(buf[i].event);
            }
            if (!(got_first && got_second)) {
                std::this_thread::yield();
            }
        }
        if (!(got_first && got_second)) {
            ++missed;
        }
        sub->cancel();
    }

    stop_noise.store(true, std::memory_order_relaxed);
    noise.clear();
    publisher_thread.request_stop();
    publisher_thread.join();

    EXPECT_EQ(missed, 0) << "of " << iterations << " iterations";
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

// --- Regression (concurrency review R6): notify_ready() must not
// have a fast path that reads on_ready_ with anything weaker than the
// seq_cst sequence on_ready()'s install uses, or "install the hook, then
// immediately poll" becomes a classic store-buffering lost wake-up - two
// threads each write their own atomic and read the other's, and relaxed
// orderings let both see only the pre-write value. One Subscription, reused
// across many short rounds (one deliver()+notify_ready() racing one
// on_ready()+poll()) with a fresh on_ready install every round, so a lost
// wake-up shows up as neither the hook firing nor the poll seeing data. ---
TEST(PublisherSubscriptionConcurrency, OnReadyNeverLosesAWakeUpUnderRacingInstall) {
    constexpr int rounds = 200'000;
    const PublishedEvent event{ShardId{0}, SequenceNumber{1}, Timestamp{1},
                               accepted_event(InstrumentId{1})};

    std::atomic<int> round{-1};
    std::atomic<int> deliver_done{-1};
    std::atomic<int> poll_done{-1};
    std::atomic<Subscription*> current{nullptr};
    std::atomic<bool> fired{false};
    std::atomic<std::size_t> last_poll_count{0};

    // on_ready() is set-once per Subscription (by design - see its doc), so
    // a fresh one is needed every round: a Subscription reused across
    // rounds would just hit that rejection on round 2, not the race this
    // test is about.
    std::jthread deliverer{[&] {
        for (int r = 0; r < rounds; ++r) {
            while (round.load(std::memory_order_acquire) != r) {
                std::this_thread::yield();
            }
            Subscription* sub = current.load(std::memory_order_acquire);
            (void)sub->deliver(event);
            sub->notify_ready();
            deliver_done.store(r, std::memory_order_release);
        }
    }};
    std::jthread poller{[&] {
        std::vector<PublishedEvent> buf(4);
        for (int r = 0; r < rounds; ++r) {
            while (round.load(std::memory_order_acquire) != r) {
                std::this_thread::yield();
            }
            Subscription* sub = current.load(std::memory_order_acquire);
            (void)sub->on_ready(
                [&fired]() noexcept { fired.store(true, std::memory_order_relaxed); });
            last_poll_count.store(sub->poll(std::span{buf}), std::memory_order_release);
            poll_done.store(r, std::memory_order_release);
        }
    }};

    int lost = 0;
    for (int r = 0; r < rounds; ++r) {
        auto sub = std::make_unique<Subscription>(SubscriptionFilter{}, 8);
        sub->set_start_sequence({SequenceNumber{0}});
        fired.store(false, std::memory_order_relaxed);
        current.store(sub.get(), std::memory_order_release);
        round.store(r, std::memory_order_release);
        while (deliver_done.load(std::memory_order_acquire) != r ||
               poll_done.load(std::memory_order_acquire) != r) {
            std::this_thread::yield();
        }
        const bool saw_data = last_poll_count.load(std::memory_order_acquire) > 0;
        const bool saw_fire = fired.load(std::memory_order_relaxed);
        if (!saw_data && !saw_fire) {
            ++lost;
        }
    }

    deliverer.join();
    poller.join();
    EXPECT_EQ(lost, 0) << "of " << rounds << " rounds";
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

// --- Regression (code review): subscribe() called before start() must not
// block forever - there is no publisher thread yet to drain the control
// queue and acknowledge the registration. A caller that does
// `engine.subscribe(...); engine.start();` on one thread (wiring up a
// subscriber before opening the engine to traffic, a real pattern) would
// otherwise deadlock itself. ---

TEST(PublisherSubscriptionPreStart, SubscribeBeforeStartThenStartDeliversInFull) {
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}}, .shard_count = 1},
                  journals.factory(), clock};

    // Engine::start() has not been called yet - this must still return
    // (not hang) and the subscription must still work once the engine does
    // start.
    auto sub = engine.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 64);
    EXPECT_TRUE(sub->is_registered());

    engine.start();

    auto [completion, reply] = test::reply_future();
    ASSERT_TRUE(engine.submit(buy(InstrumentId{1}), std::move(completion)).has_value());
    ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);

    // Checked before engine.stop(): Publisher::run's shutdown closes every
    // still-registered subscription (same observable effect as overflow),
    // which would make overflowed() legitimately true for an unrelated
    // reason after that point.
    std::vector<PublishedEvent> buf(16);
    const std::size_t n = sub->poll(std::span{buf});
    ASSERT_EQ(n, 2U);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(buf[0].event));
    EXPECT_TRUE(std::holds_alternative<BookLevelChanged>(buf[1].event));
    EXPECT_FALSE(sub->overflowed());

    engine.stop();
}

// Same scenario as above, but bounded: if the pre-start direct-registration
// path ever regresses back to blocking before a publisher thread exists,
// this fails instead of hanging the whole test binary.
TEST(PublisherSubscriptionPreStart, SubscribeThenStartOnOneThreadCompletes) {
    EXPECT_TRUE(completes_within([] {
        ManualClock clock{1'000, 10};
        test::MemoryJournals journals;
        Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}}, .shard_count = 1},
                      journals.factory(), clock};
        auto sub = engine.subscribe({}, 8);
        engine.start();
        engine.stop();
    }));
}

// --- Regression (concurrency review R7): Engine::stop() without a
// preceding start() never runs the publisher thread, so neither stopped_
// nor started_ is ever set by run() - subscribe() must still not hang (it
// takes the pre-start direct-registration path instead, see its doc), and
// Engine::stop() must still close whatever that path already registered,
// and mark it closed for anything that subscribes afterwards (see
// Publisher::close_before_start's doc) - a reactor waiting on a
// subscription has no other way to learn that nothing is ever coming. ---
TEST(PublisherSubscriptionPreStart, SubscribeAfterStopWithoutStartCompletes) {
    EXPECT_TRUE(completes_within([] {
        ManualClock clock{1'000, 10};
        test::MemoryJournals journals;
        Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}}, .shard_count = 1},
                      journals.factory(), clock};
        // Registered before stop() - task 013's reactors subscribe once at
        // startup and expect a stop() they raced to still close them, not
        // leave them silently open with no publisher thread ever coming to
        // drain or deliver to them.
        auto registered_before_stop = engine.subscribe({}, 8);
        engine.stop();
        // A subscribe() call after stop() must return an already-closed
        // subscription (the same shutdown contract as the started-then-
        // stopped case above), not one that looks live but will never
        // receive anything - a waiting reactor needs overflowed() (or an
        // on_ready() firing) to find out there is nothing more coming.
        auto sub = engine.subscribe({}, 8);
        EXPECT_TRUE(sub->is_registered());
        EXPECT_TRUE(sub->overflowed());
        EXPECT_TRUE(registered_before_stop->overflowed());
    }));
}

// --- Regression (concurrency review R8): subscribe() called from the
// publisher thread itself (e.g. from inside a completion) can never be
// served - nothing else drives the registration handshake it would
// otherwise wait on, and the wait would also wedge every shard via egress
// back-pressure (ADR-0003). Publisher::subscribe() fails fast (fatal(),
// which aborts) instead of spinning forever. ---
// Out-of-line (not a lambda passed to EXPECT_DEATH directly): the macro's
// argument parser only tracks parentheses, not braces or structured-binding
// brackets, so a statement with top-level commas - e.g. `auto [a, b] = ...`
// - splits across "arguments" it does not actually have.
void subscribe_from_publisher_thread() {
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}}, .shard_count = 1},
                  journals.factory(), clock};
    engine.start();
    // No need to wait for the reply: the process aborts from inside the
    // completion below before it could ever be set.
    (void)engine.submit(buy(InstrumentId{1}), [&engine](const CommandReply&) noexcept {
        // Runs on the publisher thread (Publisher::poll_once() invokes
        // completions inline) - exactly the case this guard exists for.
        (void)engine.subscribe({}, 8);
    });
    // If the guard above ever regresses, the completion spins forever
    // instead of aborting, engine.submit() still returns immediately (it
    // does not wait for the completion), and falling off the end of this
    // function would run ~Engine - whose stop() joins the now-permanently-
    // stuck publisher thread, hanging the whole test binary instead of
    // just failing this one death test. Bound the wait and exit the child
    // directly (bypassing destructors) so a regression here is reported as
    // "statement did not cause the child to die" instead.
    std::this_thread::sleep_for(std::chrono::seconds{5});
    std::_Exit(0);
}

TEST(PublisherSubscriptionPreStart, SubscribeFromPublisherThreadAborts) {
    // Everything - including starting the Engine's background threads - runs
    // inside the forked child (EXPECT_DEATH forks here): gtest's death tests
    // document that forking a process with other threads already running is
    // unsafe (a thread that existed only in the parent vanishes from the
    // child mid-whatever it was doing, locks included). Building the Engine
    // only in subscribe_from_publisher_thread(), called from inside the
    // statement, sidesteps that entirely.
    EXPECT_DEATH(subscribe_from_publisher_thread(), "");
}

}  // namespace
}  // namespace lockstep::app
