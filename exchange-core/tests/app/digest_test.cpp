// Digest canonical encoding (task 010): the digest must be sensitive to every
// field of every event/reply, to their order, and to book state, since it is
// the oracle lockstep-replay and --print-digest-on-exit rely on to prove two
// runs produced identical output (ADR-0004).
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/app/digest.hpp"

namespace lockstep::app {
namespace {

using namespace domain;

PublishedEvent accepted_event() {
    return PublishedEvent{
        .shard = ShardId{0},
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
    return BookSnapshot{.instrument = InstrumentId{1},
                        .bids = {LevelView{.price = Price{55}, .quantity = Quantity{10},
                                           .order_count = 1}},
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
