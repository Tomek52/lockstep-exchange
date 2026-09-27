#include "lockstep/grpc/order_entry_service.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <utility>

#include "lockstep/codec/order_entry_codec.hpp"

namespace lockstep::grpc_adapter {

grpc::Status check_protocol(const grpc::CallbackServerContext& context) {
    const auto& metadata = context.client_metadata();
    const auto it = metadata.find(protocol_metadata_key);
    if (it == metadata.end()) {
        return grpc::Status::OK;  // lenient: absent header means "current major"
    }
    const std::string_view value{it->second.data(), it->second.size()};
    unsigned major = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), major);
    if (ec != std::errc{} || major != protocol_major) {
        return {grpc::StatusCode::FAILED_PRECONDITION,
                "unsupported protocol version '" + std::string{value} + "', server speaks " +
                    std::to_string(protocol_major) + "." + std::to_string(protocol_minor)};
    }
    return grpc::Status::OK;
}

template <typename Request, typename Response>
grpc::ServerUnaryReactor* OrderEntryService::handle(grpc::CallbackServerContext* context,
                                                    const Request& request,
                                                    Response& response) {
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();

    if (grpc::Status status = check_protocol(*context); !status.ok()) {
        reactor->Finish(status);
        return reactor;
    }

    auto command = codec::decode(request);
    if (!command) {
        codec::encode_rejection(codec::to_proto(command.error()), codec::to_string(command.error()),
                                *response.mutable_ack());
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    // Runs on the publisher thread. `reactor` and `response` stay valid until
    // Finish() - which is the last thing this callback does.
    app::Completion completion = [reactor, &response](const app::CommandReply& reply) noexcept {
        codec::encode(reply, *response.mutable_ack());
        reactor->Finish(grpc::Status::OK);
    };

    const auto submitted = ingress_.submit(std::move(*command), std::move(completion));
    if (!submitted) {
        switch (submitted.error()) {
            case app::SubmitError::Overloaded:
                reactor->Finish(
                    {grpc::StatusCode::RESOURCE_EXHAUSTED, "ingress queue full, retry"});
                break;
            case app::SubmitError::ShuttingDown:
                reactor->Finish({grpc::StatusCode::UNAVAILABLE, "exchange is shutting down"});
                break;
            case app::SubmitError::UnknownInstrument:
            case app::SubmitError::NotRoutable:
                codec::encode_rejection(codec::to_proto(submitted.error()),
                                        app::to_string(submitted.error()), *response.mutable_ack());
                reactor->Finish(grpc::Status::OK);
                break;
        }
    }
    return reactor;
}

grpc::ServerUnaryReactor* OrderEntryService::SubmitOrder(grpc::CallbackServerContext* context,
                                                         const v1::SubmitOrderRequest* request,
                                                         v1::SubmitOrderResponse* response) {
    return handle(context, *request, *response);
}

grpc::ServerUnaryReactor* OrderEntryService::CancelOrder(grpc::CallbackServerContext* context,
                                                         const v1::CancelOrderRequest* request,
                                                         v1::CancelOrderResponse* response) {
    return handle(context, *request, *response);
}

grpc::ServerUnaryReactor* OrderEntryService::ModifyOrder(grpc::CallbackServerContext* context,
                                                         const v1::ModifyOrderRequest* request,
                                                         v1::ModifyOrderResponse* response) {
    return handle(context, *request, *response);
}

}  // namespace lockstep::grpc_adapter
