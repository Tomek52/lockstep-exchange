#include "options.hpp"

#include <charconv>
#include <ranges>
#include <string_view>

namespace lockstep::main_app {

namespace {

template <typename T>
std::expected<T, std::string> parse_number(std::string_view text, std::string_view what) {
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(std::string{what} + ": not a number: '" + std::string{text} + "'");
    }
    return value;
}

std::expected<std::vector<std::uint32_t>, std::string> parse_instruments(std::string_view text) {
    std::vector<std::uint32_t> ids;
    for (const auto part : text | std::views::split(',')) {
        auto id = parse_number<std::uint32_t>(std::string_view{part}, "--instruments");
        if (!id) {
            return std::unexpected(id.error());
        }
        ids.push_back(*id);
    }
    if (ids.empty()) {
        return std::unexpected(std::string{"--instruments: at least one id is required"});
    }
    return ids;
}

}  // namespace

std::expected<Options, std::string> parse_options(std::span<char* const> args) {
    Options options;
    for (const std::string_view arg : args | std::views::drop(1)) {
        const auto eq = arg.find('=');
        const std::string_view key = arg.substr(0, eq);
        const std::string_view value = eq == std::string_view::npos ? "" : arg.substr(eq + 1);

        if (key == "--help" || key == "-h") {
            options.help = true;
        } else if (key == "--listen") {
            options.listen = value;
        } else if (key == "--risk-sentinel") {
            options.risk_target = value;
        } else if (key == "--no-risk") {
            options.risk_enabled = false;
        } else if (key == "--exchange-id") {
            options.exchange_id = value;
        } else if (key == "--shards") {
            auto shards = parse_number<std::size_t>(value, "--shards");
            if (!shards || *shards == 0) {
                return std::unexpected(shards ? std::string{"--shards must be >= 1"}
                                              : shards.error());
            }
            options.shards = *shards;
        } else if (key == "--instruments") {
            auto ids = parse_instruments(value);
            if (!ids) {
                return std::unexpected(ids.error());
            }
            options.instruments = std::move(*ids);
        } else {
            return std::unexpected("unknown option '" + std::string{arg} + "'");
        }
    }
    return options;
}

}  // namespace lockstep::main_app
