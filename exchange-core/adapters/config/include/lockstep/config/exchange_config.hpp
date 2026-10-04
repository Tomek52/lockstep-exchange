#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "lockstep/domain/risk_state.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::config {

/// Display-only reference data for one instrument (ADR-0005): the human symbol
/// and the tick/lot sizes used at the edges for formatting and conversion. The
/// domain itself works in integer ticks and lots and never reads these, so
/// they are kept as strings and never parsed into floating point.
struct InstrumentMetadata {
    domain::InstrumentId id;
    std::string symbol;
    std::string tick_size;
    std::string lot_size;

    friend bool operator==(const InstrumentMetadata&, const InstrumentMetadata&) = default;
};

/// A fully parsed and validated exchange configuration. `instruments[i]`
/// describes the same instrument as `metadata[i]`; the two vectors stay
/// parallel and have equal length (ADR-0019).
struct ExchangeConfig {
    std::size_t shards{};
    domain::RiskLinkPolicy risk_link_policy{domain::RiskLinkPolicy::FailOpen};
    std::vector<domain::InstrumentSpec> instruments;
    std::vector<InstrumentMetadata> metadata;
};

/// Reads and parses a JSON configuration file (ADR-0019). On any error -
/// missing file, malformed JSON, failed validation - returns a message that
/// names the offending field. No exception escapes (ADR-0008).
[[nodiscard]] std::expected<ExchangeConfig, std::string> load_config(
    const std::filesystem::path& file);

/// Parses and validates a JSON configuration document. Validation rejects:
/// duplicate instrument ids, `min_price_ticks < 1`, `min > max`, a zero
/// quantity limit, an unknown `risk_link_policy`, `shards < 1`, and any
/// unknown key (typos must fail). The error names the offending field.
[[nodiscard]] std::expected<ExchangeConfig, std::string> parse_config(std::string_view text);

}  // namespace lockstep::config
