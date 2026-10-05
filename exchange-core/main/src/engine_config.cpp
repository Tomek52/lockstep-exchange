#include "engine_config.hpp"

#include <ranges>
#include <utility>
#include <vector>

#include "lockstep/config/exchange_config.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::main_app {

std::expected<app::EngineConfig, std::string> build_engine_config(
    const std::filesystem::path& config_file,
    std::span<const std::uint32_t> instruments,
    std::size_t shard_count) {
    if (!config_file.empty()) {
        auto config = config::load_config(config_file);
        if (!config) {
            return std::unexpected(std::move(config).error());
        }
        return app::EngineConfig{.instruments = std::move(config->instruments),
                                 .shard_count = config->shards,
                                 .risk_link_policy = config->risk_link_policy};
    }
    return app::EngineConfig{
        .instruments = instruments | std::views::transform([](std::uint32_t id) {
                           return domain::InstrumentSpec{.id = domain::InstrumentId{id}};
                       }) |
                       std::ranges::to<std::vector>(),
        .shard_count = shard_count,
    };
}

}  // namespace lockstep::main_app
