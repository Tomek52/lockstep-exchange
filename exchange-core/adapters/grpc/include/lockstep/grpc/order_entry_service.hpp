#pragma once

#include <grpcpp/grpcpp.h>

#include "lockstep/app/ports/command_ingress.hpp"
#include <lockstep/v1/order_entry.grpc.pb.h>

namespace lockstep::grpc_adapter {

/// Driving adapter: OrderEntryService over the gRPC callback API.
///
/// Each RPC is decoded on the gRPC thread, submitted to the application core
/// and answered from the completion, which runs on the publisher thread once
/// the command has been journaled and applied. No gRPC thread ever waits for a
/// shard (ADR-0003).
///
/// Outcome mapping (ADR-0006):
///  * business rejections (domain or decode)  -> OK + CommandAck.rejected
///  * back-pressure (ingress queue full)       -> RESOURCE_EXHAUSTED
///  * shutting down                            -> UNAVAILABLE
///  * protocol major version mismatch          -> FAILED_PRECONDITION
class OrderEntryService final : public v1::OrderEntryService::CallbackService {
public:
    explicit OrderEntryService(app::CommandIngress& ingress) noexcept : ingress_{ingress} {}

    grpc::ServerUnaryReactor* SubmitOrder(grpc::CallbackServerContext* context,
                                          const v1::SubmitOrderRequest* request,
                                          v1::SubmitOrderResponse* response) override;
    grpc::ServerUnaryReactor* CancelOrder(grpc::CallbackServerContext* context,
                                          const v1::CancelOrderRequest* request,
                                          v1::CancelOrderResponse* response) override;
    grpc::ServerUnaryReactor* ModifyOrder(grpc::CallbackServerContext* context,
                                          const v1::ModifyOrderRequest* request,
                                          v1::ModifyOrderResponse* response) override;

private:
    template <typename Request, typename Response>
    grpc::ServerUnaryReactor* handle(grpc::CallbackServerContext* context,
                                     const Request& request,
                                     Response& response);

    app::CommandIngress& ingress_;
};

/// Metadata header carrying "<major>.<minor>" on unary calls (see common.proto).
inline constexpr const char* protocol_metadata_key = "x-lockstep-protocol";
inline constexpr unsigned protocol_major = 1;
inline constexpr unsigned protocol_minor = 0;

/// OK if the header is absent or names protocol_major; FAILED_PRECONDITION otherwise.
[[nodiscard]] grpc::Status check_protocol(const grpc::CallbackServerContext& context);

}  // namespace lockstep::grpc_adapter
