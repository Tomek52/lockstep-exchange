#include "lockstep/grpc/market_data_service.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "lockstep/app/fatal.hpp"
#include "lockstep/app/subscription.hpp"
#include "lockstep/codec/market_data_codec.hpp"
#include "lockstep/grpc/order_entry_service.hpp"

namespace lockstep::grpc_adapter {

/// One Subscribe call. Created by Subscribe(), deleted by the dispatcher once
/// gRPC reported OnDone (the "delete in OnDone" rule of the callback API,
/// deferred to a thread that is never inside one of this stream's callbacks).
class MarketDataService::Stream final : public grpc::ServerWriteReactor<v1::SubscribeResponse> {
public:
    Stream(MarketDataService& owner, const v1::SubscribeRequest& request) : owner_{owner} {
        if (request.has_protocol() && request.protocol().major() != protocol_major) {
            early_status_ = grpc::Status{
                grpc::StatusCode::FAILED_PRECONDITION,
                "unsupported protocol version " + std::to_string(request.protocol().major()) +
                    "." + std::to_string(request.protocol().minor()) + ", server speaks " +
                    std::to_string(protocol_major) + "." + std::to_string(protocol_minor)};
            pumping_.store(true);  // nothing to pump; only Finish() below
            return;
        }
        app::SubscriptionFilter filter{.instruments = {},
                                       .trades = request.include_trades(),
                                       .book_updates = request.include_book_updates(),
                                       .private_events = false};
        filter.instruments.reserve(static_cast<std::size_t>(request.instrument_ids_size()));
        for (const std::uint32_t id : request.instrument_ids()) {
            filter.instruments.emplace_back(id);
        }
        // Blocks this gRPC thread until the publisher registered the
        // subscription (Engine::subscribe's contract), normally well under a
        // publisher iteration.
        subscription_ = owner.engine_.subscribe(std::move(filter), owner.capacity_);
    }

    /// Called once, after the stream is registered with the service.
    void start() {
        if (early_status_) {
            Finish(*early_status_);
            return;
        }
        // The hook runs on the publisher thread: flag and wake, nothing else.
        const bool installed = subscription_->on_ready([this]() noexcept {
            hook_fired_.store(true);
            owner_.signal();
        });
        if (!installed) {  // impossible: this is the first and only call
            Finish(grpc::Status{grpc::StatusCode::INTERNAL, "subscription hook"});
            return;
        }
        kick();  // events delivered before the hook existed never re-fire it
    }

    void OnWriteDone(bool ok) override {
        if (!ok) {
            Finish(grpc::Status::CANCELLED);  // transport failed or call cancelled
            return;
        }
        pump_owned();
    }

    void OnCancel() override {
        cancelled_.store(true);
        kick();
    }

    void OnDone() override {
        if (subscription_) {
            // After this returns the on_ready() hook can never run again, so
            // the stream may be destroyed. The hook never blocks and never
            // starts a gRPC operation, so this cannot wait on this thread.
            subscription_->cancel();
        }
        MarketDataService& owner = owner_;  // `this` may be deleted once done_ is set
        done_.store(true);
        owner.signal();
    }

    // --- dispatcher interface ---

    [[nodiscard]] bool done() const noexcept { return done_.load(); }
    [[nodiscard]] bool take_hook_fired() noexcept { return hook_fired_.exchange(false); }
    void request_close() noexcept {
        closing_.store(true);
        hook_fired_.store(true);
    }
    void kick() {
        again_.store(true);
        if (!pumping_.exchange(true)) {
            pump_owned();
        }
    }

private:
    /// Runs with `pumping_` held. Either starts a write or finishes the call
    /// (and keeps `pumping_` held for good), or has nothing to do, releases
    /// `pumping_` and re-checks `again_` so a wake-up that raced with the
    /// release is not lost. seq_cst throughout: `again_` and `pumping_` are
    /// two independent atomics in a store-then-load handshake on both sides.
    void pump_owned() {
        for (;;) {
            again_.store(false);
            if (step()) {
                return;  // a write or Finish() is in flight: touch nothing more
            }
            pumping_.store(false);
            if (!again_.load() || pumping_.exchange(true)) {
                return;
            }
        }
    }

    /// True if a write or Finish() was started, false if there is nothing to
    /// send. StartWrite()/Finish() must be the last thing done: gRPC may run
    /// the completion inline, and then OnWriteDone re-enters pump_owned().
    [[nodiscard]] bool step() {
        if (cancelled_.load()) {
            Finish(grpc::Status::CANCELLED);
            return true;
        }
        if (closing_.load()) {
            Finish(grpc::Status{grpc::StatusCode::UNAVAILABLE, "server shutting down"});
            return true;
        }
        for (;;) {
            // Read before polling: if the subscription had already overflowed,
            // nothing is pushed after this point, so an empty poll below means
            // every buffered event has been delivered.
            const bool overflowed = subscription_->overflowed();
            if (subscription_->poll(std::span{&event_, 1}) == 0) {
                if (overflowed) {
                    Finish(grpc::Status{grpc::StatusCode::RESOURCE_EXHAUSTED,
                                        "subscriber too slow, buffer overflowed"});
                    return true;
                }
                return false;
            }
            if (codec::encode(event_, response_)) {
                StartWrite(&response_);
                return true;
            }
        }
    }

    MarketDataService& owner_;
    std::shared_ptr<app::Subscription> subscription_;
    std::optional<grpc::Status> early_status_;
    app::PublishedEvent event_{};
    v1::SubscribeResponse response_;

    std::atomic<bool> pumping_{false};  // hand-over token: who may call step()
    std::atomic<bool> again_{false};
    std::atomic<bool> hook_fired_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> done_{false};
};

MarketDataService::MarketDataService(app::Engine& engine, std::size_t per_subscriber_capacity)
    : engine_{engine},
      capacity_{per_subscriber_capacity},
      dispatcher_{[this](const std::stop_token& stop) { run(stop); }} {}

MarketDataService::~MarketDataService() {
    dispatcher_.request_stop();
    dispatcher_.join();
    const std::scoped_lock lock{streams_mutex_};
    for (Stream* stream : streams_) {
        if (!stream->done()) {
            app::fatal("MarketDataService destroyed while a stream is still active; "
                       "shut the gRPC server down first");
        }
        delete stream;
    }
    streams_.clear();
}

grpc::ServerWriteReactor<v1::SubscribeResponse>* MarketDataService::Subscribe(
    grpc::CallbackServerContext* /*context*/, const v1::SubscribeRequest* request) {
    auto* stream = new Stream{*this, *request};
    {
        const std::scoped_lock lock{streams_mutex_};
        streams_.push_back(stream);
    }
    stream->start();
    return stream;
}

void MarketDataService::close_streams() noexcept {
    {
        const std::scoped_lock lock{streams_mutex_};
        for (Stream* stream : streams_) {
            stream->request_close();
        }
    }
    signal();
}

std::size_t MarketDataService::active_streams() const {
    const std::scoped_lock lock{streams_mutex_};
    return static_cast<std::size_t>(std::ranges::count_if(
        streams_, [](const Stream* stream) { return !stream->done(); }));
}

void MarketDataService::signal() noexcept {
    signalled_.store(true);
    signalled_.notify_one();
}

void MarketDataService::run(const std::stop_token& stop) {
    const std::stop_callback wake_on_stop{stop, [this] { signal(); }};
    while (!stop.stop_requested()) {
        signalled_.wait(false);
        signalled_.store(false);
        sweep();
    }
}

void MarketDataService::sweep() {
    std::vector<Stream*> ready;
    std::vector<Stream*> finished;
    {
        const std::scoped_lock lock{streams_mutex_};
        for (auto it = streams_.begin(); it != streams_.end();) {
            if ((*it)->done()) {
                finished.push_back(*it);
                it = streams_.erase(it);
                continue;
            }
            if ((*it)->take_hook_fired()) {
                ready.push_back(*it);
            }
            ++it;
        }
    }
    // Outside the lock: gRPC may complete a call inline while we pump it.
    // Streams in `ready` stay alive: only this thread deletes, and only
    // streams that were already done when they were collected above.
    for (Stream* stream : ready) {
        stream->kick();
    }
    for (Stream* stream : finished) {
        delete stream;
    }
}

}  // namespace lockstep::grpc_adapter
