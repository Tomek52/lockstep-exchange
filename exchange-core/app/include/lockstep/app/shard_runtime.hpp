#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <vector>

#include "lockstep/app/messages.hpp"
#include "lockstep/app/ports/clock.hpp"
#include "lockstep/app/ports/journal.hpp"
#include "lockstep/app/queues.hpp"
#include "lockstep/domain/events.hpp"
#include "lockstep/domain/shard_engine.hpp"

namespace lockstep::app {

/// One shard: the only thread that touches its ShardEngine (ADR-0003).
///
/// Loop: drain up to `max_batch` commands from ingress; for each, assign the
/// next sequence number and a timestamp, append it to the journal, apply it,
/// and stage its events and reply. Then commit the journal and only then push
/// the staged items to egress - outputs never overtake durability (ADR-0004).
class ShardRuntime {
public:
    struct Config {
        domain::ShardConfig shard;
        std::size_t ingress_capacity{4096};
        std::size_t egress_capacity{16384};
        std::size_t max_batch{256};
    };

    ShardRuntime(const Config& config, std::unique_ptr<Journal> journal, Clock& clock);

    [[nodiscard]] IngressQueue& ingress() noexcept { return ingress_; }
    [[nodiscard]] EgressQueue& egress() noexcept { return egress_; }
    [[nodiscard]] domain::ShardId shard() const noexcept { return engine_.shard(); }

    /// Thread body. Returns after `stop` is requested and ingress is drained.
    /// Precondition for a lossless stop: producers have stopped submitting.
    void run(const std::stop_token& stop);

    /// Only valid while the shard thread is not running (tests, replay checks).
    [[nodiscard]] const domain::ShardEngine& engine() const noexcept { return engine_; }

private:
    std::size_t poll_once();
    void process(InboundCommand inbound);
    void release_staged();

    domain::ShardEngine engine_;
    std::unique_ptr<Journal> journal_;
    Clock& clock_;
    std::size_t max_batch_;
    IngressQueue ingress_;
    EgressQueue egress_;
    domain::EventBuffer events_;
    std::vector<OutboundItem> staged_;
    std::uint64_t sequence_{0};
};

}  // namespace lockstep::app
