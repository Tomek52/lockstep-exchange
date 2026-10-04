// Tests for the JSON exchange-config parser (ADR-0019, task 012). A valid
// document parses to the expected ExchangeConfig, and every validation error
// the spec lists is asserted to name the offending field.
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "lockstep/config/exchange_config.hpp"
#include "lockstep/domain/risk_state.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::config {
namespace {

using domain::InstrumentId;
using domain::Price;
using domain::Quantity;
using domain::RiskLinkPolicy;

constexpr std::string_view kValid = R"({
  "shards": 2,
  "risk_link_policy": "fail_closed",
  "instruments": [
    { "id": 1, "symbol": "LKS-A", "tick_size": "0.01", "lot_size": "1",
      "min_price_ticks": 1, "max_price_ticks": 100000000, "max_order_quantity": 100000 },
    { "id": 7, "symbol": "LKS-B", "tick_size": "0.05", "lot_size": "10",
      "min_price_ticks": 5, "max_price_ticks": 200, "max_order_quantity": 50 }
  ]
})";

// Returns the error of a parse expected to fail (fails the test otherwise).
std::string parse_error(std::string_view json) {
    auto result = parse_config(json);
    EXPECT_FALSE(result.has_value()) << "expected a validation error";
    return result ? std::string{} : result.error();
}

TEST(ParseConfig, ValidDocumentParsesToExpectedConfig) {
    const auto result = parse_config(kValid);
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(result->shards, 2U);
    EXPECT_EQ(result->risk_link_policy, RiskLinkPolicy::FailClosed);

    ASSERT_EQ(result->instruments.size(), 2U);
    ASSERT_EQ(result->metadata.size(), 2U);

    const domain::InstrumentSpec expected0{.id = InstrumentId{1},
                                           .min_price = Price{1},
                                           .max_price = Price{100'000'000},
                                           .max_order_quantity = Quantity{100'000}};
    EXPECT_EQ(result->instruments[0], expected0);

    const InstrumentMetadata meta0{
        .id = InstrumentId{1}, .symbol = "LKS-A", .tick_size = "0.01", .lot_size = "1"};
    EXPECT_EQ(result->metadata[0], meta0);

    EXPECT_EQ(result->instruments[1].id, InstrumentId{7});
    EXPECT_EQ(result->instruments[1].min_price, Price{5});
    EXPECT_EQ(result->instruments[1].max_price, Price{200});
    EXPECT_EQ(result->instruments[1].max_order_quantity, Quantity{50});
    EXPECT_EQ(result->metadata[1].symbol, "LKS-B");
}

TEST(ParseConfig, DefaultsRiskLinkPolicyToFailOpenWhenAbsent) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    const auto result = parse_config(json);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->risk_link_policy, RiskLinkPolicy::FailOpen);
}

TEST(ParseConfig, RejectsDuplicateInstrumentIds) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 3, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5 },
        { "id": 3, "symbol": "B", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    const auto error = parse_error(json);
    EXPECT_NE(error.find("id"), std::string::npos) << error;
    EXPECT_NE(error.find("3"), std::string::npos) << error;
}

TEST(ParseConfig, RejectsMinPriceBelowOne) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 0, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("min_price_ticks"), std::string::npos);
}

TEST(ParseConfig, RejectsMinGreaterThanMax) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 100, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    const auto error = parse_error(json);
    EXPECT_NE(error.find("min_price_ticks"), std::string::npos) << error;
    EXPECT_NE(error.find("max_price_ticks"), std::string::npos) << error;
}

TEST(ParseConfig, RejectsZeroQuantityLimit) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 0 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("max_order_quantity"), std::string::npos);
}

TEST(ParseConfig, RejectsNegativeQuantityLimitInsteadOfWrapping) {
    // A negative integer must not static_cast-wrap into a huge unsigned value.
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": -1 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("max_order_quantity"), std::string::npos);
}

TEST(ParseConfig, RejectsFractionalQuantityLimitInsteadOfTruncating) {
    // get<std::int64_t>() would silently truncate 100.7 to 100.
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 100.7 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("max_order_quantity"), std::string::npos);
}

TEST(ParseConfig, RejectsPriceBeyondSignedRangeInsteadOfWrapping) {
    // 2^63 is a valid JSON unsigned integer that would wrap to INT64_MIN.
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 9223372036854775808, "max_order_quantity": 5 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("max_price_ticks"), std::string::npos);
}

TEST(ParseConfig, WrongTypeNamesTheField) {
    constexpr std::string_view json = R"({
      "shards": "2",
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("shards"), std::string::npos);
}

TEST(ParseConfig, NonStringSymbolNamesTheField) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 1, "symbol": 42, "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("symbol"), std::string::npos);
}

TEST(ParseConfig, RejectsNonPositiveInstrumentId) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 0, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("id"), std::string::npos);
}

TEST(ParseConfig, RejectsUnknownRiskLinkPolicy) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "risk_link_policy": "fail_sideways",
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("risk_link_policy"), std::string::npos);
}

TEST(ParseConfig, RejectsShardsBelowOne) {
    constexpr std::string_view json = R"({
      "shards": 0,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("shards"), std::string::npos);
}

TEST(ParseConfig, RejectsUnknownTopLevelKey) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "shrads": 2,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5 }
      ]
    })";
    EXPECT_NE(parse_error(json).find("shrads"), std::string::npos);
}

TEST(ParseConfig, RejectsUnknownInstrumentKey) {
    constexpr std::string_view json = R"({
      "shards": 1,
      "instruments": [
        { "id": 1, "symbol": "A", "tick_size": "0.01", "lot_size": "1",
          "min_price_ticks": 1, "max_price_ticks": 10, "max_order_quantity": 5,
          "colour": "blue" }
      ]
    })";
    EXPECT_NE(parse_error(json).find("colour"), std::string::npos);
}

TEST(ParseConfig, RejectsEmptyInstrumentList) {
    constexpr std::string_view json = R"({ "shards": 1, "instruments": [] })";
    EXPECT_NE(parse_error(json).find("instruments"), std::string::npos);
}

TEST(ParseConfig, RejectsMalformedJson) {
    EXPECT_FALSE(parse_config("{ not json").has_value());
}

TEST(LoadConfig, MissingFileReportsThePath) {
    const auto result = load_config("/nonexistent/exchange.json");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("exchange.json"), std::string::npos) << result.error();
}

}  // namespace
}  // namespace lockstep::config
