#include "lockstep/journal/record_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace lockstep::journal {
namespace {

using namespace domain;
using app::JournalError;

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const int value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

std::vector<std::byte> concat(std::vector<std::byte> head, const std::vector<std::byte>& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

std::vector<std::byte> encoded(const SequencedCommand& command) {
    std::vector<std::byte> out;
    encode_payload(command, out);
    return out;
}

CommandTag tag_of(const Command& command) {
    return std::visit([](const auto& alternative) { return alternative.tag; }, command);
}

// ---- Golden bytes (acceptance criterion 3) ---------------------------------
// Hand-written from the layout in record_codec.hpp. Every field has distinct
// byte values, so a swapped, resized or reordered field fails loudly. If one
// of these tests fails, the on-disk format changed: bump format_version
// (ADR-0012) instead of editing the expected bytes.

constexpr SequenceNumber golden_sequence{0x0102'0304'0506'0708ULL};
constexpr Timestamp golden_timestamp{0x1112'1314'1516'1718LL};

const std::vector<std::byte> golden_prefix =
    bytes({0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,    // sequence
           0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11});  // timestamp

struct Golden {
    std::string name;
    SequencedCommand command;
    std::vector<std::byte> payload;
};

SequencedCommand sequenced(Command command) {
    return SequencedCommand{golden_sequence, golden_timestamp, std::move(command)};
}

std::vector<Golden> goldens() {
    return {
        {"NewOrder",
         sequenced(NewOrder{.trader = TraderId{0x2122'2324'2526'2728ULL},
                            .client_order_id = ClientOrderId{0x3132'3334'3536'3738ULL},
                            .instrument = InstrumentId{0x4142'4344U},
                            .side = Side::Sell,
                            .type = OrderType::Market,
                            .time_in_force = TimeInForce::Ioc,
                            .price = Price{0x5152'5354'5556'5758LL},
                            .quantity = Quantity{0x6162'6364'6566'6768ULL}}),
         concat(golden_prefix,
                bytes({0x01,                                                // tag NewOrder
                       0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,      // trader
                       0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,      // client_order_id
                       0x44, 0x43, 0x42, 0x41,                              // instrument
                       0x01,                                                // side Sell
                       0x01,                                                // type Market
                       0x01,                                                // time_in_force Ioc
                       0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51,      // price
                       0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61}))},  // quantity
        {"NewOrderBuyLimitGtc",
         sequenced(NewOrder{.trader = TraderId{7},
                            .client_order_id = ClientOrderId{8},
                            .instrument = InstrumentId{9},
                            .side = Side::Buy,
                            .type = OrderType::Limit,
                            .time_in_force = TimeInForce::Gtc,
                            .price = Price{-2},
                            .quantity = Quantity{10}}),
         concat(golden_prefix,
                bytes({0x01,                                                // tag NewOrder
                       0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,      // trader
                       0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,      // client_order_id
                       0x09, 0x00, 0x00, 0x00,                              // instrument
                       0x00,                                                // side Buy
                       0x00,                                                // type Limit
                       0x00,                                                // time_in_force Gtc
                       0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,      // price -2
                       0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}))},  // quantity
        {"CancelOrder",
         sequenced(CancelOrder{.trader = TraderId{0x2122'2324'2526'2728ULL},
                               .instrument = InstrumentId{0x4142'4344U},
                               .order_id = OrderId{0x7172'7374'7576'7778ULL}}),
         concat(golden_prefix,
                bytes({0x02,                                                // tag CancelOrder
                       0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,      // trader
                       0x44, 0x43, 0x42, 0x41,                              // instrument
                       0x78, 0x77, 0x76, 0x75, 0x74, 0x73, 0x72, 0x71}))},  // order_id
        {"ModifyOrder",
         sequenced(ModifyOrder{.trader = TraderId{0x2122'2324'2526'2728ULL},
                               .instrument = InstrumentId{0x4142'4344U},
                               .order_id = OrderId{0x7172'7374'7576'7778ULL},
                               .new_price = Price{0x5152'5354'5556'5758LL},
                               .new_quantity = Quantity{0x6162'6364'6566'6768ULL}}),
         concat(golden_prefix,
                bytes({0x03,                                                // tag ModifyOrder
                       0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,      // trader
                       0x44, 0x43, 0x42, 0x41,                              // instrument
                       0x78, 0x77, 0x76, 0x75, 0x74, 0x73, 0x72, 0x71,      // order_id
                       0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51,      // new_price
                       0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61}))},  // new_quantity
        {"BlockTrader",
         sequenced(BlockTrader{.command_id = RiskCommandId{0x8182'8384'8586'8788ULL},
                               .trader = TraderId{0x2122'2324'2526'2728ULL}}),
         concat(golden_prefix,
                bytes({0x04,                                                // tag BlockTrader
                       0x88, 0x87, 0x86, 0x85, 0x84, 0x83, 0x82, 0x81,      // command_id
                       0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21}))},  // trader
        {"UnblockTrader",
         sequenced(UnblockTrader{.command_id = RiskCommandId{0x8182'8384'8586'8788ULL},
                                 .trader = TraderId{0x2122'2324'2526'2728ULL}}),
         concat(golden_prefix,
                bytes({0x05,                                                // tag UnblockTrader
                       0x88, 0x87, 0x86, 0x85, 0x84, 0x83, 0x82, 0x81,      // command_id
                       0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21}))},  // trader
        {"KillSwitch",
         sequenced(
             KillSwitch{.command_id = RiskCommandId{0x8182'8384'8586'8788ULL}, .engaged = true}),
         concat(golden_prefix,
                bytes({0x06,                                            // tag KillSwitch
                       0x88, 0x87, 0x86, 0x85, 0x84, 0x83, 0x82, 0x81,  // command_id
                       0x01}))},                                        // engaged
        {"KillSwitchReleased",
         sequenced(KillSwitch{.command_id = RiskCommandId{1}, .engaged = false}),
         concat(golden_prefix,
                bytes({0x06,                                            // tag KillSwitch
                       0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // command_id
                       0x00}))},                                        // engaged
        {"RiskLinkStatus", sequenced(RiskLinkStatus{.connected = true}),
         concat(golden_prefix, bytes({0x07,      // tag RiskLinkStatus
                                      0x01}))},  // connected
    };
}

class GoldenPayload : public ::testing::TestWithParam<Golden> {};

TEST_P(GoldenPayload, EncodesToPinnedBytes) {
    EXPECT_EQ(encoded(GetParam().command), GetParam().payload);
}

TEST_P(GoldenPayload, DecodesPinnedBytes) {
    const auto decoded = decode_payload(GetParam().payload);
    ASSERT_TRUE(decoded.has_value()) << app::to_string(decoded.error());
    EXPECT_EQ(*decoded, GetParam().command);
}

// Acceptance criterion 4: every strict prefix is a truncated payload.
TEST_P(GoldenPayload, EveryTruncationIsCorrupt) {
    const std::span<const std::byte> payload{GetParam().payload};
    for (std::size_t size = 0; size < payload.size(); ++size) {
        const auto decoded = decode_payload(payload.first(size));
        ASSERT_FALSE(decoded.has_value()) << "prefix of " << size << " bytes decoded";
        EXPECT_EQ(decoded.error(), JournalError::Corrupt);
    }
}

TEST_P(GoldenPayload, TrailingBytesAreCorrupt) {
    auto payload = GetParam().payload;
    payload.push_back(std::byte{0});
    const auto decoded = decode_payload(payload);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error(), JournalError::Corrupt);
}

INSTANTIATE_TEST_SUITE_P(EveryTag,
                         GoldenPayload,
                         ::testing::ValuesIn(goldens()),
                         [](const auto& param_info) { return param_info.param.name; });

TEST(RecordCodec, GoldensCoverEveryCommandTag) {
    std::set<CommandTag> covered;
    for (const auto& golden : goldens()) {
        covered.insert(tag_of(golden.command.command));
    }
    // One golden per alternative of the Command variant.
    EXPECT_EQ(covered.size(), std::variant_size_v<Command>);
}

// ---- Round trip (acceptance criterion 2) -----------------------------------

TEST(RecordCodec, RoundTripsExtremeValues) {
    constexpr auto u64_max = std::numeric_limits<std::uint64_t>::max();
    constexpr auto i64_min = std::numeric_limits<std::int64_t>::min();
    constexpr auto i64_max = std::numeric_limits<std::int64_t>::max();
    constexpr auto u32_max = std::numeric_limits<std::uint32_t>::max();
    const std::vector<Command> commands{
        NewOrder{.trader = TraderId{u64_max},
                 .client_order_id = ClientOrderId{0},
                 .instrument = InstrumentId{u32_max},
                 .side = Side::Sell,
                 .type = OrderType::Limit,
                 .time_in_force = TimeInForce::Ioc,
                 .price = Price{i64_min},
                 .quantity = Quantity{u64_max}},
        CancelOrder{TraderId{1}, InstrumentId{0}, OrderId{u64_max}},
        ModifyOrder{TraderId{0}, InstrumentId{u32_max}, OrderId{1}, Price{i64_max}, Quantity{0}},
        BlockTrader{RiskCommandId{u64_max}, TraderId{0}},
        UnblockTrader{RiskCommandId{0}, TraderId{u64_max}},
        KillSwitch{RiskCommandId{u64_max}, false},
        RiskLinkStatus{false},
    };
    for (const Command& command : commands) {
        const SequencedCommand original{SequenceNumber{u64_max}, Timestamp{i64_min}, command};
        const auto decoded = decode_payload(encoded(original));
        ASSERT_TRUE(decoded.has_value()) << static_cast<int>(tag_of(command));
        EXPECT_EQ(*decoded, original);
    }
}

TEST(RecordCodec, EncodeAppendsWithoutTouchingExistingBytes) {
    std::vector<std::byte> out{std::byte{0xAA}};
    const Golden golden = goldens().front();
    encode_payload(golden.command, out);
    ASSERT_EQ(out.size(), 1 + golden.payload.size());
    EXPECT_EQ(out.front(), std::byte{0xAA});
    EXPECT_TRUE(std::equal(out.begin() + 1, out.end(), golden.payload.begin()));
}

// ---- Decoder totality (acceptance criterion 4) -----------------------------

void expect_corrupt(const std::vector<std::byte>& payload) {
    const auto decoded = decode_payload(payload);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error(), JournalError::Corrupt);
}

constexpr std::size_t tag_offset = 16;

TEST(RecordCodec, UnknownTagsAreCorrupt) {
    for (const int tag : {0x00, 0x08, 0x7F, 0xFF}) {
        auto payload = goldens().front().payload;
        payload[tag_offset] = static_cast<std::byte>(tag);
        expect_corrupt(payload);
    }
}

TEST(RecordCodec, InvalidEnumBytesAreCorrupt) {
    // Offsets of side, type and time_in_force in the NewOrder golden.
    constexpr std::size_t side_offset = tag_offset + 1 + 8 + 8 + 4;
    for (const std::size_t offset : {side_offset, side_offset + 1, side_offset + 2}) {
        for (const int value : {0x02, 0xFF}) {
            auto payload = goldens().front().payload;
            payload[offset] = static_cast<std::byte>(value);
            expect_corrupt(payload);
        }
    }
}

TEST(RecordCodec, BoolsOutsideZeroOrOneAreCorrupt) {
    for (const auto& golden : goldens()) {
        const CommandTag tag = tag_of(golden.command.command);
        if (tag != CommandTag::KillSwitch && tag != CommandTag::RiskLinkStatus) {
            continue;
        }
        for (const int value : {0x02, 0x80, 0xFF}) {
            auto payload = golden.payload;
            payload.back() = static_cast<std::byte>(value);  // the bool is the last field
            expect_corrupt(payload);
        }
    }
}

// Arbitrary bytes never crash the decoder (run under asan-ubsan), and whatever
// it accepts is the canonical encoding of the decoded command.
TEST(RecordCodec, ArbitraryBytesDecodeTotallyAndCanonically) {
    std::mt19937_64 rng{20261003};
    std::uniform_int_distribution<std::size_t> size{0, 64};
    std::uniform_int_distribution<int> byte{0, 255};
    std::uniform_int_distribution<int> tag{0, 9};
    for (int i = 0; i < 20'000; ++i) {
        std::vector<std::byte> payload(size(rng));
        for (auto& b : payload) {
            b = static_cast<std::byte>(byte(rng));
        }
        if (payload.size() > tag_offset) {
            payload[tag_offset] = static_cast<std::byte>(tag(rng));  // mostly valid tags
        }
        if (const auto decoded = decode_payload(payload)) {
            EXPECT_EQ(encoded(*decoded), payload);
        }
    }
}

// ---- config_hash (ADR-0017) ------------------------------------------------

constexpr InstrumentSpec spec_a{.id = InstrumentId{1},
                                .min_price = Price{5},
                                .max_price = Price{500},
                                .max_order_quantity = Quantity{100}};
constexpr InstrumentSpec spec_b{.id = InstrumentId{2}};

constexpr std::uint64_t hash_of(std::vector<InstrumentSpec> specs,
                                RiskLinkPolicy policy = RiskLinkPolicy::FailOpen,
                                ShardId shard = ShardId{0}) {
    return config_hash(
        ShardConfig{.shard = shard, .instruments = std::move(specs), .risk_link_policy = policy});
}

static_assert(hash_of({spec_a, spec_b}) == hash_of({spec_b, spec_a}), "independent of order");
static_assert(hash_of({spec_a, spec_b}) != hash_of({spec_a, spec_b}, RiskLinkPolicy::FailClosed),
              "covers the risk link policy");
static_assert(hash_of({spec_a}) == hash_of({spec_a}, RiskLinkPolicy::FailOpen, ShardId{7}),
              "shard id has its own header field");
static_assert(hash_of({}) != hash_of({spec_b}));

TEST(ConfigHash, CoversEverySpecField) {
    const auto base = hash_of({spec_a});
    auto changed = spec_a;
    changed.id = InstrumentId{3};
    EXPECT_NE(hash_of({changed}), base);
    changed = spec_a;
    changed.min_price = Price{6};
    EXPECT_NE(hash_of({changed}), base);
    changed = spec_a;
    changed.max_price = Price{501};
    EXPECT_NE(hash_of({changed}), base);
    changed = spec_a;
    changed.max_order_quantity = Quantity{101};
    EXPECT_NE(hash_of({changed}), base);
}

// Pinned values, computed independently from ADR-0017's definition (FNV-1a
// 64 over the little-endian byte string). A change here is a format change.
TEST(ConfigHash, MatchesPinnedValues) {
    EXPECT_EQ(hash_of({}), 0xE4BC'4FD9'252B'E94FULL);
    EXPECT_EQ(hash_of({spec_b, spec_a}, RiskLinkPolicy::FailClosed), 0xCD1F'986A'29C3'C8BAULL);
}

}  // namespace
}  // namespace lockstep::journal
