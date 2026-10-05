// Digest canonical encoding (task 010): the digest must be sensitive to every
// field of every event/reply, to their order, and to book state, since it is
// the oracle lockstep-replay and --print-digest-on-exit rely on to prove two
// runs produced identical output (ADR-0004).
#include "lockstep/app/digest.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace lockstep::app {
namespace {

using namespace domain;

PublishedEvent accepted_event() {
    return PublishedEvent{.shard = ShardId{0},
                          .sequence = SequenceNumber{1},
                          .timestamp = Timestamp{100},
                          .event = OrderAccepted{.order_id = OrderId{7},
                                                 .trader = TraderId{3},
                                                 .client_order_id = ClientOrderId{9},
                                                 .instrument = InstrumentId{1},
                                                 .side = Side::Buy,
                                                 .type = OrderType::Limit,
                                                 .price = Price{55},
                                                 .quantity = Quantity{10}}};
}

CommandReply ok_reply() {
    return CommandReply{.shard = ShardId{0},
                        .sequence = SequenceNumber{1},
                        .timestamp = Timestamp{100},
                        .result = CommandOutcome{.order_id = OrderId{7}}};
}

BookSnapshot one_level_book() {
    return BookSnapshot{
        .instrument = InstrumentId{1},
        .bids = {LevelView{.price = Price{55}, .quantity = Quantity{10}, .order_count = 1}},
        .asks = {}};
}

ReplayOutput base_output() {
    ReplayOutput output;
    output.events.push_back(accepted_event());
    output.replies.push_back(ok_reply());
    return output;
}

TEST(Digest, StableAcrossRepeatedCalls) {
    const ReplayOutput output = base_output();
    const std::vector<BookSnapshot> books{one_level_book()};
    EXPECT_EQ(digest(output, books), digest(output, books));
}

TEST(Digest, ChangesWhenAnEventFieldChanges) {
    const std::vector<BookSnapshot> books{one_level_book()};
    const ReplayOutput base = base_output();
    const std::uint64_t base_digest = digest(base, books);

    // Flip one field (price) of the one event; every other byte is identical.
    ReplayOutput changed = base;
    auto& accepted = std::get<OrderAccepted>(changed.events.front().event);
    accepted.price = Price{accepted.price.value() + 1};
    EXPECT_NE(digest(changed, books), base_digest);
}

TEST(Digest, ChangesWhenEventOrderChanges) {
    const std::vector<BookSnapshot> books{};
    ReplayOutput base;
    base.events.push_back(accepted_event());
    PublishedEvent second = accepted_event();
    second.sequence = SequenceNumber{2};
    std::get<OrderAccepted>(second.event).order_id = OrderId{8};
    base.events.push_back(second);
    const std::uint64_t base_digest = digest(base, books);

    ReplayOutput reordered;
    reordered.events.push_back(second);
    reordered.events.push_back(accepted_event());
    EXPECT_NE(digest(reordered, books), base_digest);
}

TEST(Digest, ChangesWhenAReplyFieldChanges) {
    const std::vector<BookSnapshot> books{};
    ReplayOutput base = base_output();
    base.events.clear();
    const std::uint64_t base_digest = digest(base, books);

    ReplayOutput changed = base;
    changed.replies.front().result = std::unexpected(RejectReason::InvalidPrice);
    EXPECT_NE(digest(changed, books), base_digest);
}

TEST(Digest, ChangesWhenABookSnapshotChanges) {
    const ReplayOutput output = base_output();
    const std::vector<BookSnapshot> books{one_level_book()};
    const std::uint64_t base_digest = digest(output, books);

    std::vector<BookSnapshot> changed_books = books;
    changed_books.front().bids.front().quantity = Quantity{11};
    EXPECT_NE(digest(output, changed_books), base_digest);

    std::vector<BookSnapshot> extra_level_books = books;
    extra_level_books.front().asks.push_back(
        LevelView{.price = Price{60}, .quantity = Quantity{5}, .order_count = 1});
    EXPECT_NE(digest(output, extra_level_books), base_digest);
}

// ---- Per-field coverage (task 010 review F4) -------------------------------
//
// One fixture exercising every event alternative, both reply outcomes, and a
// two-level book, then a table of mutators - one per field - each of which
// must change the digest when applied to a fresh copy of the fixture. This
// is what the five mutants below (and any future one dropping a fold) get
// caught by.

struct Fixture {
    ReplayOutput output;
    std::vector<BookSnapshot> books;
};

Fixture field_coverage_fixture() {
    Fixture f;
    f.output.events.push_back(
        PublishedEvent{.shard = ShardId{0},
                       .sequence = SequenceNumber{1},
                       .timestamp = Timestamp{100},
                       .event = OrderAccepted{.order_id = OrderId{7},
                                              .trader = TraderId{3},
                                              .client_order_id = ClientOrderId{9},
                                              .instrument = InstrumentId{1},
                                              .side = Side::Buy,
                                              .type = OrderType::Limit,
                                              .price = Price{55},
                                              .quantity = Quantity{10}}});
    f.output.events.push_back(
        PublishedEvent{.shard = ShardId{0},
                       .sequence = SequenceNumber{2},
                       .timestamp = Timestamp{101},
                       .event = OrderCancelled{.order_id = OrderId{7},
                                               .trader = TraderId{3},
                                               .instrument = InstrumentId{1},
                                               .cancelled_quantity = Quantity{4},
                                               .reason = CancelReason::UserRequested}});
    f.output.events.push_back(PublishedEvent{.shard = ShardId{0},
                                             .sequence = SequenceNumber{3},
                                             .timestamp = Timestamp{102},
                                             .event = OrderModified{.order_id = OrderId{7},
                                                                    .trader = TraderId{3},
                                                                    .instrument = InstrumentId{1},
                                                                    .price = Price{60},
                                                                    .quantity = Quantity{6},
                                                                    .kept_priority = true}});
    f.output.events.push_back(PublishedEvent{.shard = ShardId{0},
                                             .sequence = SequenceNumber{4},
                                             .timestamp = Timestamp{103},
                                             .event = Trade{.instrument = InstrumentId{1},
                                                            .price = Price{58},
                                                            .quantity = Quantity{3},
                                                            .aggressor_side = Side::Buy,
                                                            .maker_order = OrderId{7},
                                                            .maker_trader = TraderId{3},
                                                            .taker_order = OrderId{8},
                                                            .taker_trader = TraderId{4}}});
    f.output.events.push_back(
        PublishedEvent{.shard = ShardId{0},
                       .sequence = SequenceNumber{5},
                       .timestamp = Timestamp{104},
                       .event = BookLevelChanged{.instrument = InstrumentId{1},
                                                 .side = Side::Sell,
                                                 .price = Price{61},
                                                 .quantity = Quantity{12}}});
    f.output.events.push_back(PublishedEvent{
        .shard = ShardId{0},
        .sequence = SequenceNumber{6},
        .timestamp = Timestamp{105},
        .event = InstrumentStatusChanged{.instrument = InstrumentId{1}, .halted = false}});
    f.output.events.push_back(
        PublishedEvent{.shard = ShardId{0},
                       .sequence = SequenceNumber{7},
                       .timestamp = Timestamp{106},
                       .event = RiskCommandApplied{.command_id = RiskCommandId{42}}});

    // One reply per event's command (same sequence, matching the real
    // one-reply-per-command invariant app::replay() guarantees), plus an
    // eighth command that produced no event - a pure rejection.
    for (std::uint64_t sequence = 1; sequence <= 7; ++sequence) {
        f.output.replies.push_back(
            CommandReply{.shard = ShardId{0},
                         .sequence = SequenceNumber{sequence},
                         .timestamp = Timestamp{static_cast<std::int64_t>(99 + sequence)},
                         .result = CommandOutcome{.order_id = OrderId{sequence}}});
    }
    f.output.replies.push_back(CommandReply{.shard = ShardId{0},
                                            .sequence = SequenceNumber{8},
                                            .timestamp = Timestamp{107},
                                            .result = std::unexpected(RejectReason::InvalidPrice)});

    f.books.push_back(BookSnapshot{
        .instrument = InstrumentId{1},
        .bids = {LevelView{.price = Price{55}, .quantity = Quantity{10}, .order_count = 1}},
        .asks = {LevelView{.price = Price{61}, .quantity = Quantity{12}, .order_count = 2}}});
    return f;
}

struct FieldCase {
    std::string name;
    std::function<void(Fixture&)> mutate;
};

template <typename Event>
Event& event_at(Fixture& f, std::size_t index) {
    return std::get<Event>(f.output.events.at(index).event);
}

std::vector<FieldCase> field_cases() {
    return {
        // PublishedEvent wrapper fields (any one event stands in for all).
        {"PublishedEvent.shard", [](Fixture& f) { f.output.events[0].shard = ShardId{1}; }},
        {"PublishedEvent.sequence",
         [](Fixture& f) { f.output.events[0].sequence = SequenceNumber{99}; }},
        {"PublishedEvent.timestamp",
         [](Fixture& f) { f.output.events[0].timestamp = Timestamp{999}; }},

        // OrderAccepted (events[0]).
        {"OrderAccepted.order_id",
         [](Fixture& f) { event_at<OrderAccepted>(f, 0).order_id = OrderId{999}; }},
        {"OrderAccepted.trader",
         [](Fixture& f) { event_at<OrderAccepted>(f, 0).trader = TraderId{999}; }},
        {"OrderAccepted.client_order_id",
         [](Fixture& f) { event_at<OrderAccepted>(f, 0).client_order_id = ClientOrderId{999}; }},
        {"OrderAccepted.instrument",
         [](Fixture& f) { event_at<OrderAccepted>(f, 0).instrument = InstrumentId{999}; }},
        {"OrderAccepted.side", [](Fixture& f) { event_at<OrderAccepted>(f, 0).side = Side::Sell; }},
        {"OrderAccepted.type",
         [](Fixture& f) { event_at<OrderAccepted>(f, 0).type = OrderType::Market; }},
        {"OrderAccepted.price",
         [](Fixture& f) { event_at<OrderAccepted>(f, 0).price = Price{999}; }},
        {"OrderAccepted.quantity",
         [](Fixture& f) { event_at<OrderAccepted>(f, 0).quantity = Quantity{999}; }},

        // OrderCancelled (events[1]).
        {"OrderCancelled.order_id",
         [](Fixture& f) { event_at<OrderCancelled>(f, 1).order_id = OrderId{999}; }},
        {"OrderCancelled.trader",
         [](Fixture& f) { event_at<OrderCancelled>(f, 1).trader = TraderId{999}; }},
        {"OrderCancelled.instrument",
         [](Fixture& f) { event_at<OrderCancelled>(f, 1).instrument = InstrumentId{999}; }},
        {"OrderCancelled.cancelled_quantity",
         [](Fixture& f) { event_at<OrderCancelled>(f, 1).cancelled_quantity = Quantity{999}; }},
        {"OrderCancelled.reason",
         [](Fixture& f) { event_at<OrderCancelled>(f, 1).reason = CancelReason::KillSwitch; }},

        // OrderModified (events[2]).
        {"OrderModified.order_id",
         [](Fixture& f) { event_at<OrderModified>(f, 2).order_id = OrderId{999}; }},
        {"OrderModified.trader",
         [](Fixture& f) { event_at<OrderModified>(f, 2).trader = TraderId{999}; }},
        {"OrderModified.instrument",
         [](Fixture& f) { event_at<OrderModified>(f, 2).instrument = InstrumentId{999}; }},
        {"OrderModified.price",
         [](Fixture& f) { event_at<OrderModified>(f, 2).price = Price{999}; }},
        {"OrderModified.quantity",
         [](Fixture& f) { event_at<OrderModified>(f, 2).quantity = Quantity{999}; }},
        {"OrderModified.kept_priority",
         [](Fixture& f) { event_at<OrderModified>(f, 2).kept_priority = false; }},

        // Trade (events[3]).
        {"Trade.instrument",
         [](Fixture& f) { event_at<Trade>(f, 3).instrument = InstrumentId{999}; }},
        {"Trade.price", [](Fixture& f) { event_at<Trade>(f, 3).price = Price{999}; }},
        {"Trade.quantity", [](Fixture& f) { event_at<Trade>(f, 3).quantity = Quantity{999}; }},
        {"Trade.aggressor_side",
         [](Fixture& f) { event_at<Trade>(f, 3).aggressor_side = Side::Sell; }},
        {"Trade.maker_order", [](Fixture& f) { event_at<Trade>(f, 3).maker_order = OrderId{999}; }},
        {"Trade.maker_trader",
         [](Fixture& f) { event_at<Trade>(f, 3).maker_trader = TraderId{999}; }},
        {"Trade.taker_order", [](Fixture& f) { event_at<Trade>(f, 3).taker_order = OrderId{999}; }},
        {"Trade.taker_trader",
         [](Fixture& f) { event_at<Trade>(f, 3).taker_trader = TraderId{999}; }},

        // BookLevelChanged (events[4]).
        {"BookLevelChanged.instrument",
         [](Fixture& f) { event_at<BookLevelChanged>(f, 4).instrument = InstrumentId{999}; }},
        {"BookLevelChanged.side",
         [](Fixture& f) { event_at<BookLevelChanged>(f, 4).side = Side::Buy; }},
        {"BookLevelChanged.price",
         [](Fixture& f) { event_at<BookLevelChanged>(f, 4).price = Price{999}; }},
        {"BookLevelChanged.quantity",
         [](Fixture& f) { event_at<BookLevelChanged>(f, 4).quantity = Quantity{999}; }},

        // InstrumentStatusChanged (events[5]).
        {"InstrumentStatusChanged.instrument",
         [](Fixture& f) {
             event_at<InstrumentStatusChanged>(f, 5).instrument = InstrumentId{999};
         }},
        {"InstrumentStatusChanged.halted",
         [](Fixture& f) { event_at<InstrumentStatusChanged>(f, 5).halted = true; }},

        // RiskCommandApplied (events[6]).
        {"RiskCommandApplied.command_id",
         [](Fixture& f) { event_at<RiskCommandApplied>(f, 6).command_id = RiskCommandId{999}; }},

        // CommandReply fields: once via an ok reply (replies[0]), once via
        // the rejected one (replies[7], the eighth command) for the
        // error-path fields.
        {"CommandReply.shard (ok)", [](Fixture& f) { f.output.replies[0].shard = ShardId{1}; }},
        {"CommandReply.sequence (ok)",
         [](Fixture& f) { f.output.replies[0].sequence = SequenceNumber{999}; }},
        {"CommandReply.timestamp (ok)",
         [](Fixture& f) { f.output.replies[0].timestamp = Timestamp{999}; }},
        {"CommandReply.result.order_id (ok)",
         [](Fixture& f) { f.output.replies[0].result = CommandOutcome{.order_id = OrderId{999}}; }},
        {"CommandReply.shard (rejected)",
         [](Fixture& f) { f.output.replies[7].shard = ShardId{1}; }},
        {"CommandReply.sequence (rejected)",
         [](Fixture& f) { f.output.replies[7].sequence = SequenceNumber{999}; }},
        {"CommandReply.timestamp (rejected)",
         [](Fixture& f) { f.output.replies[7].timestamp = Timestamp{999}; }},
        {"CommandReply.result.error (rejected)",
         [](Fixture& f) {
             f.output.replies[7].result = std::unexpected(RejectReason::TradingHalted);
         }},
        {"CommandReply ok<->rejected discriminant",
         [](Fixture& f) {
             f.output.replies[0].result = std::unexpected(RejectReason::InvalidPrice);
         }},

        // BookSnapshot / LevelView fields.
        {"BookSnapshot.instrument", [](Fixture& f) { f.books[0].instrument = InstrumentId{999}; }},
        {"LevelView.price (bid)", [](Fixture& f) { f.books[0].bids[0].price = Price{999}; }},
        {"LevelView.quantity (bid)",
         [](Fixture& f) { f.books[0].bids[0].quantity = Quantity{999}; }},
        {"LevelView.order_count (bid)", [](Fixture& f) { f.books[0].bids[0].order_count = 999; }},
        {"LevelView.price (ask)", [](Fixture& f) { f.books[0].asks[0].price = Price{999}; }},
        {"LevelView.quantity (ask)",
         [](Fixture& f) { f.books[0].asks[0].quantity = Quantity{999}; }},
        {"LevelView.order_count (ask)", [](Fixture& f) { f.books[0].asks[0].order_count = 999; }},
    };
}

TEST(Digest, EveryFieldChangesTheDigest) {
    const Fixture base = field_coverage_fixture();
    const std::uint64_t base_digest = digest(base.output, base.books);
    for (const FieldCase& field_case : field_cases()) {
        Fixture mutated = base;
        field_case.mutate(mutated);
        EXPECT_NE(digest(mutated.output, mutated.books), base_digest) << field_case.name;
    }
}

// Pinned golden value: any unintended change to the canonical encoding
// (field order, byte width, count placement) changes this and must be
// treated as a digest format change, not quietly updated.
TEST(Digest, GoldenValueForAFixedSmallInput) {
    const Fixture f = field_coverage_fixture();
    EXPECT_EQ(digest(f.output, f.books), 0x55CB'F564'27EE'EEF1ULL);
}

TEST(Digest, ChangesWhenBookCountChangesEvenIfBytesCoincide) {
    // Guards the length-prefix rationale in digest.cpp: an empty extra book
    // appended at the end must still change the digest.
    const ReplayOutput output = base_output();
    const std::vector<BookSnapshot> one_book{one_level_book()};
    std::vector<BookSnapshot> two_books = one_book;
    two_books.push_back(BookSnapshot{.instrument = InstrumentId{2}, .bids = {}, .asks = {}});
    EXPECT_NE(digest(output, one_book), digest(output, two_books));
}

}  // namespace
}  // namespace lockstep::app
