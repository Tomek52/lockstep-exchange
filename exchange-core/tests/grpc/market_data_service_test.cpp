// In-process gRPC test of MarketDataService against a real Engine (shards,
// journal, publisher) over a loopback channel.
#include "lockstep/grpc/market_data_service.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "lockstep/grpc/server.hpp"

#include "app/test_support.hpp"

namespace lockstep::grpc_adapter {
namespace {

using namespace std::chrono_literals;

constexpr auto call_deadline = 20s;  // a hang fails the test instead of the build
constexpr std::size_t small_buffer = 4;

/// One client side stream, with a deadline so that no read can hang forever.
struct Client {
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReader<v1::SubscribeResponse>> reader;

    /// Reads the next message; false when the stream ended.
    bool next(v1::SubscribeResponse& out) { return reader->Read(&out); }
};

class MarketDataServiceTest : public ::testing::Test {
protected:
    app::ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    app::Engine engine{app::EngineConfig{.instruments = {{.id = domain::InstrumentId{1}},
                                                         {.id = domain::InstrumentId{2}}},
                                         .shard_count = 1},
                       journals.factory(), clock};
    MarketDataService service{engine, small_buffer};
    GrpcServer server{"127.0.0.1:0", {&service}};
    std::unique_ptr<v1::MarketDataService::Stub> stub =
        v1::MarketDataService::NewStub(grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));
    std::size_t opened = 0;

    void SetUp() override { engine.start(); }
    void TearDown() override {
        service.close_streams();
        server.shutdown(5s);
        engine.stop();
    }

    static v1::SubscribeRequest request(std::vector<std::uint32_t> instruments,
                                        bool trades = true,
                                        bool book = true) {
        v1::SubscribeRequest req;
        req.mutable_protocol()->set_major(1);
        for (const std::uint32_t id : instruments) {
            req.add_instrument_ids(id);
        }
        req.set_include_trades(trades);
        req.set_include_book_updates(book);
        return req;
    }

    /// Opens a stream and waits until the engine registered its subscription,
    /// so every command submitted afterwards is seen by it.
    std::unique_ptr<Client> open(const v1::SubscribeRequest& req) {
        auto client = std::make_unique<Client>();
        client->context.set_deadline(std::chrono::system_clock::now() + call_deadline);
        client->reader = stub->Subscribe(&client->context, req);
        ++opened;
        const auto give_up = std::chrono::steady_clock::now() + 10s;
        while (service.active_streams() < opened && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_EQ(service.active_streams(), opened) << "stream never registered";
        return client;
    }

    void place(domain::InstrumentId instrument,
               domain::Side side,
               std::int64_t price,
               std::uint64_t quantity,
               std::uint32_t trader) {
        auto [completion, reply] = test::reply_future();
        const domain::NewOrder order{.trader = domain::TraderId{trader},
                                     .client_order_id = domain::ClientOrderId{++client_order_id_},
                                     .instrument = instrument,
                                     .side = side,
                                     .type = domain::OrderType::Limit,
                                     .time_in_force = domain::TimeInForce::Gtc,
                                     .price = domain::Price{price},
                                     .quantity = domain::Quantity{quantity}};
        ASSERT_TRUE(engine.submit(order, std::move(completion)).has_value());
        ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    }

private:
    std::uint64_t client_order_id_ = 0;
};

constexpr domain::InstrumentId inst1{1};
constexpr domain::InstrumentId inst2{2};

TEST_F(MarketDataServiceTest, SubscriberReceivesPublicEventsInShardSequenceOrder) {
    auto client = open(request({}));

    place(inst1, domain::Side::Sell, 100, 10, /*trader=*/11);
    place(inst1, domain::Side::Buy, 100, 4, /*trader=*/22);

    // Expected: a level appears (qty 10), then the cross: a trade of 4 and the
    // level shrinks to 6. Events of one command share its shard sequence.
    std::vector<v1::SubscribeResponse> received;
    bool saw_trade = false;
    bool saw_shrunk_level = false;
    v1::SubscribeResponse message;
    while (!(saw_trade && saw_shrunk_level) && client->next(message)) {
        received.push_back(message);
        if (message.has_trade()) {
            saw_trade = true;
            EXPECT_EQ(message.trade().instrument_id(), 1U);
            EXPECT_EQ(message.trade().price_ticks(), 100);
            EXPECT_EQ(message.trade().quantity(), 4U);
            EXPECT_EQ(message.trade().aggressor_side(), v1::SIDE_BUY);
        }
        if (message.has_book_level_update() && message.book_level_update().quantity() == 6) {
            saw_shrunk_level = true;
        }
    }
    EXPECT_TRUE(saw_trade);
    EXPECT_TRUE(saw_shrunk_level);
    ASSERT_GE(received.size(), 3U);

    for (std::size_t i = 1; i < received.size(); ++i) {
        EXPECT_EQ(received[i].shard_id(), received[0].shard_id());
        EXPECT_GE(received[i].shard_sequence(), received[i - 1].shard_sequence())
            << "events must arrive in shard sequence order";
        EXPECT_GE(received[i].timestamp_ns(), received[i - 1].timestamp_ns());
    }
    EXPECT_LT(received.front().shard_sequence(), received.back().shard_sequence());
    client->context.TryCancel();
}

TEST_F(MarketDataServiceTest, InstrumentFilterExcludesOtherInstruments) {
    auto client = open(request({2}));

    place(inst1, domain::Side::Sell, 100, 10, 11);  // filtered out
    place(inst2, domain::Side::Sell, 200, 7, 11);
    place(inst1, domain::Side::Buy, 100, 10, 22);  // filtered out (a trade on 1)

    v1::SubscribeResponse message;
    ASSERT_TRUE(client->next(message));
    // The stream is ordered, so had instrument 1 leaked through, its level
    // update (submitted first) would be the first message.
    ASSERT_TRUE(message.has_book_level_update());
    EXPECT_EQ(message.book_level_update().instrument_id(), 2U);
    EXPECT_EQ(message.book_level_update().price_ticks(), 200);
    EXPECT_EQ(message.book_level_update().quantity(), 7U);
    client->context.TryCancel();
}

TEST_F(MarketDataServiceTest, KindFlagsSelectTradesAndBookUpdates) {
    auto trades_only = open(request({}, /*trades=*/true, /*book=*/false));

    place(inst1, domain::Side::Sell, 100, 10, 11);  // book update only: not delivered
    place(inst1, domain::Side::Buy, 100, 3, 22);

    v1::SubscribeResponse message;
    ASSERT_TRUE(trades_only->next(message));
    EXPECT_TRUE(message.has_trade()) << "book updates must not be sent";
    trades_only->context.TryCancel();
}

TEST_F(MarketDataServiceTest, SlowSubscriberIsDisconnectedWhileAnotherKeepsReceiving) {
    constexpr std::uint32_t resting_orders = 300;
    // Trades only, so the setup below produces nothing for this stream.
    auto slow = open(request({1}, /*trades=*/true, /*book=*/false));
    auto healthy = open(request({2}));

    for (std::uint32_t i = 0; i < resting_orders; ++i) {
        place(inst1, domain::Side::Sell, 100, 1, /*trader=*/100 + i);
    }
    // One command that yields `resting_orders` trades in a single publisher
    // flush: far more than the 4 events the slow stream can buffer, while the
    // stream is not being read.
    place(inst1, domain::Side::Buy, 100, resting_orders, /*trader=*/1);
    place(inst2, domain::Side::Sell, 300, 5, /*trader=*/2);

    v1::SubscribeResponse message;
    ASSERT_TRUE(healthy->next(message)) << "the other subscriber must keep receiving";
    EXPECT_EQ(message.book_level_update().instrument_id(), 2U);
    EXPECT_EQ(message.book_level_update().price_ticks(), 300);

    std::uint32_t trades_seen = 0;
    while (slow->next(message)) {
        ASSERT_TRUE(message.has_trade());
        ++trades_seen;
    }
    const grpc::Status status = slow->reader->Finish();
    EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED) << status.error_message();
    EXPECT_LT(trades_seen, resting_orders) << "events were dropped, the client must be told";

    // The healthy stream is still alive and receives later events.
    place(inst2, domain::Side::Sell, 301, 5, 2);
    ASSERT_TRUE(healthy->next(message));
    EXPECT_EQ(message.book_level_update().price_ticks(), 301);
    healthy->context.TryCancel();
}

TEST_F(MarketDataServiceTest, IncompatibleProtocolMajorIsRefused) {
    auto req = request({});
    req.mutable_protocol()->set_major(2);
    Client client;
    client.context.set_deadline(std::chrono::system_clock::now() + call_deadline);
    client.reader = stub->Subscribe(&client.context, req);

    v1::SubscribeResponse message;
    EXPECT_FALSE(client.next(message));
    EXPECT_EQ(client.reader->Finish().error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(MarketDataServiceTest, ClientCancelReleasesTheStream) {
    auto client = open(request({}));
    ASSERT_EQ(service.active_streams(), 1U);
    client->context.TryCancel();

    const auto give_up = std::chrono::steady_clock::now() + 10s;
    while (service.active_streams() != 0 && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(service.active_streams(), 0U);

    // The engine keeps publishing to nobody without trouble.
    place(inst1, domain::Side::Sell, 100, 1, 11);
}

TEST_F(MarketDataServiceTest, ShutdownWithActiveSubscribersCompletes) {
    auto reading = open(request({}));
    auto idle = open(request({1}));
    auto never_read = open(request({2}));

    // Shut down on another thread so a hang fails the test (after the wait)
    // instead of hanging it.
    std::promise<void> finished;
    auto finished_future = finished.get_future();
    std::thread shutdown{[&] {
        service.close_streams();
        server.shutdown(10s);
        finished.set_value();
    }};
    const bool in_time = finished_future.wait_for(15s) == std::future_status::ready;
    if (in_time) {
        shutdown.join();
    } else {
        shutdown.detach();
    }
    ASSERT_TRUE(in_time) << "shutdown hung with active subscribers";

    v1::SubscribeResponse message;
    EXPECT_FALSE(reading->next(message));
    EXPECT_EQ(reading->reader->Finish().error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_FALSE(idle->next(message));
    EXPECT_EQ(idle->reader->Finish().error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_FALSE(never_read->next(message));
    EXPECT_EQ(never_read->reader->Finish().error_code(), grpc::StatusCode::UNAVAILABLE);
}

}  // namespace
}  // namespace lockstep::grpc_adapter
