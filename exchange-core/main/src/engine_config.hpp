#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>

#include "lockstep/app/engine.hpp"

namespace lockstep::main_app {

/// Builds the engine configuration either from a JSON config file
/// (ADR-0019) - the single source of instruments, shard count and risk
/// link policy - or, when `config_file` is empty, from `instruments` and
/// `shard_count` (the --instruments/--shards-style flags) with their
/// defaults. Shared by exchange-core's main.cpp and lockstep-replay's
/// replay_main.cpp (task 010 review F11), so the two tools' notion of how a
/// configuration resolves to shard/instrument assignment cannot drift
/// apart - the instrument -> shard assignment (app::Router::round_robin)
/// both build on top of this must agree for a journal directory to mean
/// the same thing to both.
[[nodiscard]] std::expected<app::EngineConfig, std::string> build_engine_config(
    const std::filesystem::path& config_file,
    std::span<const std::uint32_t> instruments,
    std::size_t shard_count);

}  // namespace lockstep::main_app
