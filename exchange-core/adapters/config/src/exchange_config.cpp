// JSON exchange-config parser (ADR-0019). nlohmann-json throws on malformed
// input and on type mismatches; this file is the boundary that catches those
// and turns them, together with our own validation, into an
// std::expected<..., std::string> whose error names the offending field
// (ADR-0008). No exception escapes into the app or domain.
#include "lockstep/config/exchange_config.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace lockstep::config {
namespace {

using json = nlohmann::json;

// Keys we accept. Anything else is a typo and must fail (ADR-0019): strict
// parsing keeps the operator's file and the engine's config byte-identical,
// which the journal config hash relies on (ADR-0017).
constexpr std::array top_level_keys{"shards", "risk_link_policy", "instruments"};
constexpr std::array instrument_keys{"id",
                                     "symbol",
                                     "tick_size",
                                     "lot_size",
                                     "min_price_ticks",
                                     "max_price_ticks",
                                     "max_order_quantity"};

template <std::size_t N>
[[nodiscard]] std::optional<std::string> find_unknown_key(const json& object,
                                                          const std::array<const char*, N>& allowed,
                                                          std::string_view where) {
    for (const auto& [key, value] : object.items()) {
        bool known = false;
        for (const char* candidate : allowed) {
            if (key == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            return "unknown key '" + key + "' in " + std::string{where};
        }
    }
    return std::nullopt;
}

// Reads an integer field strictly. get<std::int64_t>() alone would truncate
// a fractional literal (100.7 -> 100), wrap an unsigned literal above
// INT64_MAX, and report a type mismatch without naming the field.
[[nodiscard]] std::expected<std::int64_t, std::string> read_integer(const json& object,
                                                                    const char* key,
                                                                    std::string_view where) {
    const auto& value = object.at(key);
    const auto field = std::string{where} + key;
    if (!value.is_number_integer()) {
        return std::unexpected(field + ": must be an integer");
    }
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(field + ": out of range");
    }
    return value.get<std::int64_t>();
}

[[nodiscard]] std::expected<std::string, std::string> read_string(const json& object,
                                                                  const char* key,
                                                                  std::string_view where) {
    const auto& value = object.at(key);
    if (!value.is_string()) {
        return std::unexpected(std::string{where} + key + ": must be a string");
    }
    return value.get<std::string>();
}

[[nodiscard]] std::expected<domain::RiskLinkPolicy, std::string> parse_policy(const json& root) {
    if (!root.contains("risk_link_policy")) {
        return domain::RiskLinkPolicy::FailOpen;  // documented default (ADR-0013)
    }
    const auto& value = root.at("risk_link_policy");
    if (!value.is_string()) {
        return std::unexpected(std::string{"risk_link_policy: must be a string"});
    }
    const auto text = value.get<std::string>();
    if (text == "fail_open") {
        return domain::RiskLinkPolicy::FailOpen;
    }
    if (text == "fail_closed") {
        return domain::RiskLinkPolicy::FailClosed;
    }
    return std::unexpected("risk_link_policy: unknown value '" + text +
                           "' (expected 'fail_open' or 'fail_closed')");
}

[[nodiscard]] std::expected<void, std::string> parse_instrument(
    const json& item,
    std::size_t index,
    std::unordered_set<std::uint32_t>& seen_ids,
    ExchangeConfig& out) {
    const std::string where = "instruments[" + std::to_string(index) + "]";
    if (!item.is_object()) {
        return std::unexpected(where + ": must be an object");
    }
    if (auto unknown = find_unknown_key(item, instrument_keys, where)) {
        return std::unexpected(*unknown);
    }
    for (const char* required : instrument_keys) {
        if (!item.contains(required)) {
            return std::unexpected(where + ": missing key '" + std::string{required} + "'");
        }
    }

    // Read every numeric field as signed and range-check it. nlohmann would
    // otherwise static_cast a negative JSON literal into an unsigned field,
    // silently wrapping e.g. "max_order_quantity": -1 into a huge value that
    // sails past the ==0 check. Reject such inputs at parse time (ADR-0019).
    const std::string prefix = where + ".";
    const auto id_field = read_integer(item, "id", prefix);
    if (!id_field) {
        return std::unexpected(id_field.error());
    }
    const auto id = *id_field;
    if (id < 1 || id > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(where + ": id must be in [1, 4294967295], got " +
                               std::to_string(id));
    }
    const auto id_u = static_cast<std::uint32_t>(id);
    if (!seen_ids.insert(id_u).second) {
        return std::unexpected(where + ": duplicate instrument id " + std::to_string(id_u));
    }

    const auto min_price_field = read_integer(item, "min_price_ticks", prefix);
    const auto max_price_field = read_integer(item, "max_price_ticks", prefix);
    const auto max_qty_field = read_integer(item, "max_order_quantity", prefix);
    for (const auto* field : {&min_price_field, &max_price_field, &max_qty_field}) {
        if (!*field) {
            return std::unexpected(field->error());
        }
    }
    const auto min_price = *min_price_field;
    const auto max_price = *max_price_field;
    const auto max_qty = *max_qty_field;

    if (min_price < 1) {
        return std::unexpected(where + ": min_price_ticks must be >= 1, got " +
                               std::to_string(min_price));
    }
    if (min_price > max_price) {
        return std::unexpected(where + ": min_price_ticks (" + std::to_string(min_price) +
                               ") must be <= max_price_ticks (" + std::to_string(max_price) + ")");
    }
    if (max_qty < 1) {
        return std::unexpected(where + ": max_order_quantity must be >= 1, got " +
                               std::to_string(max_qty));
    }
    // No upper bound beyond the signed range: a notional that overflows i64
    // (max_price_ticks * max_order_quantity) is caught at runtime by the
    // domain's checked arithmetic (ADR-0005), not here.

    auto symbol = read_string(item, "symbol", prefix);
    auto tick_size = read_string(item, "tick_size", prefix);
    auto lot_size = read_string(item, "lot_size", prefix);
    for (const auto* field : {&symbol, &tick_size, &lot_size}) {
        if (!*field) {
            return std::unexpected(field->error());
        }
    }

    out.instruments.push_back(domain::InstrumentSpec{
        .id = domain::InstrumentId{id_u},
        .min_price = domain::Price{min_price},
        .max_price = domain::Price{max_price},
        .max_order_quantity = domain::Quantity{static_cast<std::uint64_t>(max_qty)}});
    out.metadata.push_back(InstrumentMetadata{.id = domain::InstrumentId{id_u},
                                              .symbol = std::move(*symbol),
                                              .tick_size = std::move(*tick_size),
                                              .lot_size = std::move(*lot_size)});
    return {};
}

[[nodiscard]] std::expected<ExchangeConfig, std::string> parse_root(const json& root) {
    if (!root.is_object()) {
        return std::unexpected(std::string{"top level: must be a JSON object"});
    }
    if (auto unknown = find_unknown_key(root, top_level_keys, "the document")) {
        return std::unexpected(*unknown);
    }
    if (!root.contains("shards")) {
        return std::unexpected(std::string{"shards: missing key"});
    }
    if (!root.contains("instruments")) {
        return std::unexpected(std::string{"instruments: missing key"});
    }

    ExchangeConfig config;

    const auto shards_field = read_integer(root, "shards", "");
    if (!shards_field) {
        return std::unexpected(shards_field.error());
    }
    const auto shards = *shards_field;
    if (shards < 1) {
        return std::unexpected("shards must be >= 1, got " + std::to_string(shards));
    }
    config.shards = static_cast<std::size_t>(shards);

    auto policy = parse_policy(root);
    if (!policy) {
        return std::unexpected(std::move(policy).error());
    }
    config.risk_link_policy = *policy;

    const auto& instruments = root.at("instruments");
    if (!instruments.is_array()) {
        return std::unexpected(std::string{"instruments: must be an array"});
    }
    if (instruments.empty()) {
        return std::unexpected(std::string{"instruments: at least one instrument is required"});
    }

    std::unordered_set<std::uint32_t> seen_ids;
    for (std::size_t i = 0; i < instruments.size(); ++i) {
        if (auto parsed = parse_instrument(instruments[i], i, seen_ids, config); !parsed) {
            return std::unexpected(std::move(parsed).error());
        }
    }
    return config;
}

}  // namespace

std::expected<ExchangeConfig, std::string> parse_config(std::string_view text) {
    // allow_exceptions=false so a parse error returns a discarded value we can
    // report, rather than throwing. parse_root checks every type before
    // reading it; the catch below is a safety net so nothing escapes (ADR-0008).
    json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        return std::unexpected(std::string{"invalid JSON: could not parse the document"});
    }
    try {
        return parse_root(root);
    } catch (const json::exception& e) {
        return std::unexpected(std::string{"invalid value: "} + e.what());
    }
}

std::expected<ExchangeConfig, std::string> load_config(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return std::unexpected("cannot open config file '" + file.string() + "'");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return std::unexpected("cannot read config file '" + file.string() + "'");
    }
    return parse_config(buffer.str());
}

}  // namespace lockstep::config
