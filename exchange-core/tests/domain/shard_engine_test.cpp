#include "lockstep/domain/shard_engine.hpp"

#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace lockstep::domain {
namespace {

constexpr InstrumentId instrument{3};

SequencedCommand sequenced(Command command, std::uint64_t seq = 1) {
    return SequencedCommand{SequenceNumber{seq}, Timestamp{1'000 * static_cast<std::int64_t>(seq)},
                            std::move(command)};
}

NewOrder buy(std::int64_t price, std::uint64_t qty, std::uint64_t client_order_id = 7) {
    return NewOrder{.trader = TraderId{42},
                    .client_order_id = ClientOrderId{client_order_id},
                    .instrument = instrument,
                    .side = Side::Buy,
                    .type = OrderType::Limit,
                    .time_in_force = TimeInForce::Gtc,
                    .price = Price{price},
                    .quantity = Quantity{qty}};
}

class ShardEngineTest : public ::testing::Test {
protected:
    ShardEngine engine{ShardConfig{.shard = ShardId{2}, .instruments = {{.id = instrument}}}};
    EventBuffer out;
};

TEST_F(ShardEngineTest, AcceptsValidOrderWithShardScopedId) {
    const CommandResult result = engine.apply(sequenced(buy(100, 10)), out);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->order_id.value() >> ShardEngine::order_id_shard_shift, 2U);
    // Non-crossing GTC limit: OrderAccepted, then BookLevelChanged as it rests
    // (task 002 matching).
    ASSERT_EQ(out.size(), 2U);
    const auto& accepted = std::get<OrderAccepted>(out.events()[0]);
    EXPECT_EQ(accepted.order_id, result->order_id);
    EXPECT_EQ(accepted.trader, TraderId{42});
    EXPECT_EQ(accepted.quantity, Quantity{10});
}

TEST_F(ShardEngineTest, OrderIdsAreMonotonicWithinAShard) {
    // Distinct client order ids: same-trader reuse of one while it is still
    // resting is a DuplicateClientOrderId reject (task 003), and this test is
    // about order id monotonicity, not that rule.
    const auto first = engine.apply(sequenced(buy(100, 1, 7), 1), out);
    const auto second = engine.apply(sequenced(buy(100, 1, 8), 2), out);
    ASSERT_TRUE(first && second);
    EXPECT_LT(first->order_id, second->order_id);
}

TEST_F(ShardEngineTest, RejectsUnknownInstrumentWithoutEvents) {
    NewOrder order = buy(100, 10);
    order.instrument = InstrumentId{999};
    EXPECT_EQ(engine.apply(sequenced(order), out).error(), RejectReason::UnknownInstrument);
    EXPECT_TRUE(out.empty());
}

TEST_F(ShardEngineTest, RejectsInvalidOrderWithoutConsumingAnId) {
    EXPECT_EQ(engine.apply(sequenced(buy(100, 0)), out).error(), RejectReason::InvalidQuantity);
    const auto accepted = engine.apply(sequenced(buy(100, 1)), out);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->order_id.value() & ((1ULL << ShardEngine::order_id_shard_shift) - 1), 1U);
}

TEST_F(ShardEngineTest, CancelOfUnknownOrderIsRejected) {
    const CancelOrder cancel{TraderId{42}, instrument, OrderId{12345}};
    EXPECT_EQ(engine.apply(sequenced(cancel), out).error(), RejectReason::UnknownOrder);
}

TEST_F(ShardEngineTest, RiskCommandsAreAcknowledgedPerShard) {
    // task 004 gave KillSwitch its real behaviour: engaging it halts every
    // instrument the shard owns (InstrumentStatusChanged), even with nothing
    // resting to cancel, before the ack. Full detail (idempotency, resting
    // orders, both directions) lives in risk_controls_test.cpp.
    const auto result = engine.apply(sequenced(KillSwitch{RiskCommandId{9}, true}), out);
    ASSERT_TRUE(result.has_value());
    const std::vector<Event> expected{InstrumentStatusChanged{instrument, true},
                                      RiskCommandApplied{RiskCommandId{9}}};
    EXPECT_EQ(std::vector<Event>(out.events().begin(), out.events().end()), expected);
}

}  // namespace
}  // namespace lockstep::domain
