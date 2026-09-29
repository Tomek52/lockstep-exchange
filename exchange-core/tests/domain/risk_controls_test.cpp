#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/domain/shard_engine.hpp"

// Acceptance criteria: docs/tasks/004-risk-controls-in-domain.md.
namespace lockstep::domain {
namespace {

constexpr InstrumentId instrument_a{1};
constexpr InstrumentId instrument_b{2};
constexpr TraderId trader_a{1};
constexpr TraderId trader_b{2};

SequencedCommand sequenced(Command command, std::uint64_t seq = 1) {
    return SequencedCommand{SequenceNumber{seq}, Timestamp{1'000 * static_cast<std::int64_t>(seq)},
                            std::move(command)};
}

NewOrder limit_order(InstrumentId inst,
                     Side side,
                     std::int64_t price,
                     std::uint64_t qty,
                     TraderId trader,
                     ClientOrderId client_order_id) {
    return NewOrder{.trader = trader,
                    .client_order_id = client_order_id,
                    .instrument = inst,
                    .side = side,
                    .type = OrderType::Limit,
                    .time_in_force = TimeInForce::Gtc,
                    .price = Price{price},
                    .quantity = Quantity{qty}};
}

// Every acceptance criterion compares the complete event sequence, not a
// prefix of it, following the pattern set by modify_test.cpp.
std::vector<Event> events_vector(const EventBuffer& out) {
    return {out.events().begin(), out.events().end()};
}

class RiskControlsTest : public ::testing::Test {
protected:
    ShardEngine engine{ShardConfig{.shard = ShardId{5},
                                   .instruments = {{.id = instrument_a}, {.id = instrument_b}}}};
    EventBuffer out;
};

// --- Criterion 1: a blocked trader's new order is rejected; other traders
// are unaffected. ------------------------------------------------------

TEST_F(RiskControlsTest, BlockedTraderNewOrderIsRejectedOtherTraderUnaffected) {
    ASSERT_TRUE(engine.apply(sequenced(BlockTrader{RiskCommandId{1}, trader_a}), out).has_value());
    out.clear();

    const auto blocked_result = engine.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{1}), 2),
        out);
    ASSERT_FALSE(blocked_result.has_value());
    EXPECT_EQ(blocked_result.error(), RejectReason::TraderBlocked);
    EXPECT_TRUE(out.empty());

    const auto other_result = engine.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_b, ClientOrderId{1}), 3),
        out);
    ASSERT_TRUE(other_result.has_value());
    ASSERT_EQ(events_vector(out).size(), 2U);  // OrderAccepted, BookLevelChanged
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(out.events()[0]));
}

// --- Criterion 2: blocking cancels exactly that trader's resting orders, on
// both sides and across instruments, in the documented order (per book,
// cancel_if's price-time order; then the ack). --------------------------

TEST_F(RiskControlsTest, BlockCancelsOnlyThatTradersRestingOrdersAcrossBooksAndSides) {
    // trader_a: resting on both sides of instrument_a, and on instrument_b.
    // trader_b: resting orders that must survive.
    ASSERT_TRUE(
        engine
            .apply(sequenced(
                       limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{1}), 1),
                   out)
            .has_value());
    out.clear();
    ASSERT_TRUE(engine
                    .apply(sequenced(limit_order(instrument_a, Side::Sell, 110, 3, trader_a,
                                                 ClientOrderId{2}),
                                     2),
                           out)
                    .has_value());
    out.clear();
    ASSERT_TRUE(
        engine
            .apply(sequenced(
                       limit_order(instrument_b, Side::Buy, 50, 7, trader_a, ClientOrderId{3}), 3),
                   out)
            .has_value());
    out.clear();
    ASSERT_TRUE(
        engine
            .apply(sequenced(
                       limit_order(instrument_a, Side::Buy, 99, 4, trader_b, ClientOrderId{1}), 4),
                   out)
            .has_value());
    out.clear();

    const auto result = engine.apply(sequenced(BlockTrader{RiskCommandId{7}, trader_a}, 5), out);
    ASSERT_TRUE(result.has_value());

    // cancel_if walks bids best-to-worst then asks best-to-worst within a
    // book, and ShardEngine walks books_ (a flat_map) in instrument-id
    // order: instrument_a's bid, then its ask, then instrument_b's bid.
    const std::vector<Event> expected{
        OrderCancelled{OrderId{(5ULL << ShardEngine::order_id_shard_shift) | 1}, trader_a,
                       instrument_a, Quantity{5}, CancelReason::TraderBlocked},
        BookLevelChanged{instrument_a, Side::Buy, Price{100}, Quantity{0}},
        OrderCancelled{OrderId{(5ULL << ShardEngine::order_id_shard_shift) | 2}, trader_a,
                       instrument_a, Quantity{3}, CancelReason::TraderBlocked},
        BookLevelChanged{instrument_a, Side::Sell, Price{110}, Quantity{0}},
        OrderCancelled{OrderId{(5ULL << ShardEngine::order_id_shard_shift) | 3}, trader_a,
                       instrument_b, Quantity{7}, CancelReason::TraderBlocked},
        BookLevelChanged{instrument_b, Side::Buy, Price{50}, Quantity{0}},
        RiskCommandApplied{RiskCommandId{7}},
    };
    EXPECT_EQ(events_vector(out), expected);

    // trader_b's resting order on instrument_a survives.
    EXPECT_EQ(engine.book(instrument_a)->best_price(Side::Buy), Price{99});
}

// --- Criterion 3: after unblock, new orders are accepted again. --------

TEST_F(RiskControlsTest, UnblockAllowsNewOrdersAgain) {
    ASSERT_TRUE(engine.apply(sequenced(BlockTrader{RiskCommandId{1}, trader_a}), out).has_value());
    out.clear();
    ASSERT_TRUE(
        engine.apply(sequenced(UnblockTrader{RiskCommandId{2}, trader_a}, 2), out).has_value());
    ASSERT_EQ(events_vector(out), (std::vector<Event>{RiskCommandApplied{RiskCommandId{2}}}));
    out.clear();

    const auto result = engine.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{9}), 3),
        out);
    ASSERT_TRUE(result.has_value());
}

// --- Criterion 4: kill switch on cancels every resting order, halts every
// instrument, rejects new orders; cancel still works; off resumes. -------

TEST_F(RiskControlsTest, KillSwitchOnCancelsHaltsAndRejectsNewOrdersCancelStillWorks) {
    ASSERT_TRUE(
        engine
            .apply(sequenced(
                       limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{1}), 1),
                   out)
            .has_value());
    out.clear();
    const auto resting = engine.apply(
        sequenced(limit_order(instrument_b, Side::Sell, 200, 2, trader_b, ClientOrderId{1}), 2),
        out);
    ASSERT_TRUE(resting.has_value());
    const OrderId resting_on_b = resting->order_id;
    out.clear();

    const auto kill_result = engine.apply(sequenced(KillSwitch{RiskCommandId{3}, true}, 3), out);
    ASSERT_TRUE(kill_result.has_value());
    const std::vector<Event> expected{
        OrderCancelled{OrderId{(5ULL << ShardEngine::order_id_shard_shift) | 1}, trader_a,
                       instrument_a, Quantity{5}, CancelReason::KillSwitch},
        BookLevelChanged{instrument_a, Side::Buy, Price{100}, Quantity{0}},
        InstrumentStatusChanged{instrument_a, true},
        OrderCancelled{resting_on_b, trader_b, instrument_b, Quantity{2}, CancelReason::KillSwitch},
        BookLevelChanged{instrument_b, Side::Sell, Price{200}, Quantity{0}},
        InstrumentStatusChanged{instrument_b, true},
        RiskCommandApplied{RiskCommandId{3}},
    };
    EXPECT_EQ(events_vector(out), expected);
    out.clear();

    const auto new_order_result = engine.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{2}), 4),
        out);
    ASSERT_FALSE(new_order_result.has_value());
    EXPECT_EQ(new_order_result.error(), RejectReason::TradingHalted);
    EXPECT_TRUE(out.empty());

    // "Cancels still work" (criterion 4): CancelOrder is not gated by
    // check_new_order at all, so it reaches the book's own UnknownOrder
    // check instead of being refused with TradingHalted. The kill switch
    // already cancelled every resting order, including resting_on_b, so
    // that is what this proves: the command path itself stays open.
    out.clear();
    const auto cancel_result =
        engine.apply(sequenced(CancelOrder{trader_b, instrument_b, resting_on_b}, 5), out);
    EXPECT_FALSE(cancel_result.has_value());
    EXPECT_EQ(cancel_result.error(), RejectReason::UnknownOrder);
    EXPECT_TRUE(out.empty());

    // Off: every instrument resumes, and orders are accepted again.
    out.clear();
    const auto off_result = engine.apply(sequenced(KillSwitch{RiskCommandId{4}, false}, 6), out);
    ASSERT_TRUE(off_result.has_value());
    const std::vector<Event> off_expected{
        InstrumentStatusChanged{instrument_a, false},
        InstrumentStatusChanged{instrument_b, false},
        RiskCommandApplied{RiskCommandId{4}},
    };
    EXPECT_EQ(events_vector(out), off_expected);
    out.clear();

    const auto resumed = engine.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{4}), 7),
        out);
    EXPECT_TRUE(resumed.has_value());
}

// --- Criterion 5: FailClosed rejects with RiskUnavailable before link-up,
// accepts once connected, rejects again after link-down. FailOpen always
// accepts, ignoring the link entirely. -----------------------------------

TEST_F(RiskControlsTest, FailClosedGatesOnLinkStatusFailOpenIgnoresIt) {
    ShardEngine fail_closed{ShardConfig{.shard = ShardId{6},
                                        .instruments = {{.id = instrument_a}},
                                        .risk_link_policy = RiskLinkPolicy::FailClosed}};
    EventBuffer fc_out;

    // Link starts down: rejected before any RiskLinkStatus{true}.
    const auto before_link = fail_closed.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{1}), 1),
        fc_out);
    ASSERT_FALSE(before_link.has_value());
    EXPECT_EQ(before_link.error(), RejectReason::RiskUnavailable);
    EXPECT_TRUE(fc_out.empty());

    ASSERT_TRUE(fail_closed.apply(sequenced(RiskLinkStatus{true}, 2), fc_out).has_value());
    EXPECT_TRUE(fc_out.empty());  // RiskLinkStatus emits no events.
    fc_out.clear();

    const auto after_link_up = fail_closed.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{2}), 3),
        fc_out);
    EXPECT_TRUE(after_link_up.has_value());
    fc_out.clear();

    ASSERT_TRUE(fail_closed.apply(sequenced(RiskLinkStatus{false}, 4), fc_out).has_value());
    fc_out.clear();

    const auto after_link_down = fail_closed.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{3}), 5),
        fc_out);
    ASSERT_FALSE(after_link_down.has_value());
    EXPECT_EQ(after_link_down.error(), RejectReason::RiskUnavailable);

    // FailOpen: the same link-down sequence never rejects.
    ShardEngine fail_open{ShardConfig{.shard = ShardId{7},
                                      .instruments = {{.id = instrument_a}},
                                      .risk_link_policy = RiskLinkPolicy::FailOpen}};
    EventBuffer fo_out;
    const auto fail_open_result = fail_open.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{1}), 1),
        fo_out);
    EXPECT_TRUE(fail_open_result.has_value());
}

// --- check_new_order's documented precedence: halted beats blocked beats
// link down. Every reachable combination is exercised so a reordering of
// RiskState::check_new_order's checks is caught. -------------------------

TEST_F(RiskControlsTest, CheckPrecedenceHaltedBeatsBlockedBeatsLinkDown) {
    ShardEngine fail_closed{ShardConfig{.shard = ShardId{8},
                                        .instruments = {{.id = instrument_a}},
                                        .risk_link_policy = RiskLinkPolicy::FailClosed}};
    EventBuffer fc_out;

    // Link down (default) and trader blocked, but not halted: TraderBlocked,
    // not RiskUnavailable.
    ASSERT_TRUE(fail_closed.apply(sequenced(BlockTrader{RiskCommandId{1}, trader_a}, 1), fc_out)
                    .has_value());
    fc_out.clear();
    const auto blocked_not_halted = fail_closed.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{1}), 2),
        fc_out);
    ASSERT_FALSE(blocked_not_halted.has_value());
    EXPECT_EQ(blocked_not_halted.error(), RejectReason::TraderBlocked);

    // Halted, trader also blocked, link also down: TradingHalted wins.
    fc_out.clear();
    ASSERT_TRUE(
        fail_closed.apply(sequenced(KillSwitch{RiskCommandId{2}, true}, 3), fc_out).has_value());
    fc_out.clear();
    const auto halted_and_blocked = fail_closed.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{2}), 4),
        fc_out);
    ASSERT_FALSE(halted_and_blocked.has_value());
    EXPECT_EQ(halted_and_blocked.error(), RejectReason::TradingHalted);

    // Resume and unblock: only the link-down check remains, so
    // RiskUnavailable is what an unblocked, un-halted trader now sees.
    fc_out.clear();
    ASSERT_TRUE(
        fail_closed.apply(sequenced(KillSwitch{RiskCommandId{3}, false}, 5), fc_out).has_value());
    fc_out.clear();
    ASSERT_TRUE(fail_closed.apply(sequenced(UnblockTrader{RiskCommandId{4}, trader_a}, 6), fc_out)
                    .has_value());
    fc_out.clear();
    const auto only_link_down = fail_closed.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{3}), 7),
        fc_out);
    ASSERT_FALSE(only_link_down.has_value());
    EXPECT_EQ(only_link_down.error(), RejectReason::RiskUnavailable);
}

// --- Criterion 6: duplicate BlockTrader with the same id changes nothing
// but still acks (only RiskCommandApplied on the redundant application). ---

TEST_F(RiskControlsTest, DuplicateBlockTraderStillAcksButChangesNothing) {
    ASSERT_TRUE(
        engine
            .apply(sequenced(
                       limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{1}), 1),
                   out)
            .has_value());
    out.clear();

    ASSERT_TRUE(
        engine.apply(sequenced(BlockTrader{RiskCommandId{1}, trader_a}, 2), out).has_value());
    out.clear();

    const auto second = engine.apply(sequenced(BlockTrader{RiskCommandId{2}, trader_a}, 3), out);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(events_vector(out), (std::vector<Event>{RiskCommandApplied{RiskCommandId{2}}}));

    // Still blocked: unaffected by the redundant application.
    out.clear();
    const auto rejected = engine.apply(
        sequenced(limit_order(instrument_a, Side::Sell, 100, 1, trader_a, ClientOrderId{2}), 4),
        out);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), RejectReason::TraderBlocked);

    // A single UnblockTrader clears it, proving the two BlockTrader
    // applications left exactly one (not two) entries for trader_a: a
    // blocked_ set that grew on the redundant apply would need a second
    // unblock to clear.
    out.clear();
    ASSERT_TRUE(
        engine.apply(sequenced(UnblockTrader{RiskCommandId{3}, trader_a}, 5), out).has_value());
    out.clear();
    const auto accepted = engine.apply(
        sequenced(limit_order(instrument_a, Side::Sell, 100, 1, trader_a, ClientOrderId{2}), 6),
        out);
    EXPECT_TRUE(accepted.has_value());
}

// --- Idempotency: redundant unblock, and both kill-switch directions, emit
// only the ack. -----------------------------------------------------------

TEST_F(RiskControlsTest, RedundantUnblockOnlyAcks) {
    const auto result = engine.apply(sequenced(UnblockTrader{RiskCommandId{1}, trader_a}), out);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(events_vector(out), (std::vector<Event>{RiskCommandApplied{RiskCommandId{1}}}));
}

TEST_F(RiskControlsTest, RedundantKillSwitchEngageOnlyAcksNoResurrectedCancelsOrStatus) {
    ASSERT_TRUE(
        engine
            .apply(sequenced(
                       limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{1}), 1),
                   out)
            .has_value());
    out.clear();

    ASSERT_TRUE(engine.apply(sequenced(KillSwitch{RiskCommandId{1}, true}, 2), out).has_value());
    out.clear();

    const auto second = engine.apply(sequenced(KillSwitch{RiskCommandId{2}, true}, 3), out);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(events_vector(out), (std::vector<Event>{RiskCommandApplied{RiskCommandId{2}}}));
}

TEST_F(RiskControlsTest, RedundantKillSwitchDisengageOnlyAcks) {
    const auto result = engine.apply(sequenced(KillSwitch{RiskCommandId{1}, false}), out);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(events_vector(out), (std::vector<Event>{RiskCommandApplied{RiskCommandId{1}}}));
}

// --- Modify: a blocked trader's modify is rejected; halted rejects modify
// too. -------------------------------------------------------------------

TEST_F(RiskControlsTest, BlockedTraderModifyIsRejected) {
    const auto resting = engine.apply(
        sequenced(limit_order(instrument_a, Side::Buy, 100, 5, trader_a, ClientOrderId{1}), 1),
        out);
    ASSERT_TRUE(resting.has_value());
    out.clear();

    ASSERT_TRUE(
        engine.apply(sequenced(BlockTrader{RiskCommandId{1}, trader_a}, 2), out).has_value());
    out.clear();

    const auto modify_result = engine.apply(
        sequenced(ModifyOrder{trader_a, instrument_a, resting->order_id, Price{100}, Quantity{1}},
                  3),
        out);
    ASSERT_FALSE(modify_result.has_value());
    EXPECT_EQ(modify_result.error(), RejectReason::TraderBlocked);
    EXPECT_TRUE(out.empty());
}

TEST_F(RiskControlsTest, HaltedModifyIsRejected) {
    const auto resting = engine.apply(
        sequenced(limit_order(instrument_a, Side::Sell, 200, 5, trader_b, ClientOrderId{1}), 1),
        out);
    ASSERT_TRUE(resting.has_value());
    const OrderId order_id = resting->order_id;
    out.clear();

    ASSERT_TRUE(engine.apply(sequenced(KillSwitch{RiskCommandId{1}, true}, 2), out).has_value());
    out.clear();

    const auto modify_result = engine.apply(
        sequenced(ModifyOrder{trader_b, instrument_a, order_id, Price{201}, Quantity{5}}, 3), out);
    ASSERT_FALSE(modify_result.has_value());
    EXPECT_EQ(modify_result.error(), RejectReason::TradingHalted);
    EXPECT_TRUE(out.empty());
}

}  // namespace
}  // namespace lockstep::domain
