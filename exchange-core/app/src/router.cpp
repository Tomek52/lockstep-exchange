#include "lockstep/app/router.hpp"

#include <algorithm>
#include <ranges>
#include <variant>

namespace lockstep::app {

using domain::InstrumentId;
using domain::InstrumentSpec;
using domain::ShardId;

Router Router::round_robin(std::span<const InstrumentSpec> instruments, std::size_t shard_count) {
    auto sorted = instruments | std::ranges::to<std::vector>();
    std::ranges::sort(sorted, {}, &InstrumentSpec::id);

    Router router;
    router.by_shard_.resize(std::max<std::size_t>(shard_count, 1));
    for (const auto [index, spec] : std::views::enumerate(sorted)) {
        const auto shard = static_cast<std::size_t>(index) % router.by_shard_.size();
        router.shard_of_.try_emplace(spec.id, ShardId{static_cast<std::uint32_t>(shard)});
        router.by_shard_[shard].push_back(spec);
    }
    return router;
}

std::optional<ShardId> Router::shard_for(InstrumentId instrument) const {
    const auto it = shard_of_.find(instrument);
    if (it == shard_of_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::span<const InstrumentSpec> Router::instruments_of(ShardId shard) const {
    return by_shard_.at(shard.value());
}

std::optional<InstrumentId> instrument_of(const domain::Command& command) noexcept {
    return std::visit(
        []<typename C>(const C& cmd) -> std::optional<InstrumentId> {
            if constexpr (requires { cmd.instrument; }) {
                return cmd.instrument;
            } else {
                return std::nullopt;
            }
        },
        command);
}

}  // namespace lockstep::app
