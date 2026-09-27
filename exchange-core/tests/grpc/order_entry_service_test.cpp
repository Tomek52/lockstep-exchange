// In-process gRPC test of the OrderEntryService adapter against a fake
// application core (the CommandIngress port), over a real loopback channel.
#include "lockstep/grpc/order_entry_service.hpp"

#include <memory>
#include <optional>
#include <string>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "lockstep/grpc/server.hpp"

namespace lockstep::grpc_adapter {
namespace {

/// Answers synchronously from inside submit(), which the callback API allows.
class FakeIngress final : public app::CommandIngress {
public:
    std::optional<app::SubmitError> fail_with;
    std::optional<domain::Command> last;

    std::expected<void, app::SubmitError> submit(domain::Command command,
                                                 app::Completion completion) override {
        if (fail_with) {
            return std::unexpected(*fail_with);
        }
        last = command;
        completion(app::CommandReply{domain::ShardId{1}, domain::SequenceNumber{7},
                                     domain::Timestamp{123},
                                     domain::CommandOutcome{domain::OrderId{99}}});
        return {};
    }
    std::expected<void, app::SubmitError> broadcast(domain::Command /*command*/) override {
        return {};
    }
};

class OrderEntryServiceTest : public ::testing::Test {
protected:
    FakeIngress ingress;
    OrderEntryService service{ingress};
    GrpcServer server{"127.0.0.1:0", {&service}};
    std::unique_ptr<v1::OrderEntryService::Stub> stub =
        v1::OrderEntryService::NewStub(grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));

    void TearDown() override { server.shutdown(std::chrono::milliseconds{100}); }

    static v1::SubmitOrderRequest valid_request() {
        v1::SubmitOrderRequest request;
        request.set_trader_id(1);
        request.set_client_order_id(2);
        request.set_instrument_id(3);
        request.set_side(v1::SIDE_BUY);
        request.set_type(v1::ORDER_TYPE_LIMIT);
        request.set_time_in_force(v1::TIME_IN_FORCE_GTC);
        request.set_price_ticks(100);
        request.set_quantity(10);
        return request;
    }
};

TEST_F(OrderEntryServiceTest, AcceptedOrderReturnsShardStampedAck) {
    grpc::ClientContext context;
    v1::SubmitOrderResponse response;
    const grpc::Status status = stub->SubmitOrder(&context, valid_request(), &response);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.ack().accepted().order_id(), 99U);
    EXPECT_EQ(response.ack().shard_id(), 1U);
    EXPECT_EQ(response.ack().shard_sequence(), 7U);
    ASSERT_TRUE(ingress.last.has_value());
    EXPECT_EQ(std::get<domain::NewOrder>(*ingress.last).quantity, domain::Quantity{10});
}

TEST_F(OrderEntryServiceTest, MalformedOrderIsRejectedWithoutReachingTheCore) {
    auto request = valid_request();
    request.set_side(v1::SIDE_UNSPECIFIED);
    grpc::ClientContext context;
    v1::SubmitOrderResponse response;

    ASSERT_TRUE(stub->SubmitOrder(&context, request, &response).ok());
    EXPECT_EQ(response.ack().rejected().reason(), v1::REJECT_REASON_INVALID_SIDE);
    EXPECT_EQ(response.ack().shard_sequence(), 0U) << "never sequenced";
    EXPECT_FALSE(ingress.last.has_value());
}

TEST_F(OrderEntryServiceTest, BackPressureMapsToResourceExhausted) {
    ingress.fail_with = app::SubmitError::Overloaded;
    grpc::ClientContext context;
    v1::SubmitOrderResponse response;
    EXPECT_EQ(stub->SubmitOrder(&context, valid_request(), &response).error_code(),
              grpc::StatusCode::RESOURCE_EXHAUSTED);
}

TEST_F(OrderEntryServiceTest, IncompatibleProtocolMajorIsRefused) {
    grpc::ClientContext context;
    context.AddMetadata(protocol_metadata_key, "2.0");
    v1::SubmitOrderResponse response;
    EXPECT_EQ(stub->SubmitOrder(&context, valid_request(), &response).error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(OrderEntryServiceTest, CompatibleProtocolHeaderIsAccepted) {
    grpc::ClientContext context;
    context.AddMetadata(protocol_metadata_key, "1.3");
    v1::SubmitOrderResponse response;
    EXPECT_TRUE(stub->SubmitOrder(&context, valid_request(), &response).ok());
}

}  // namespace
}  // namespace lockstep::grpc_adapter
