#include "lockstep/grpc/server.hpp"

#include <stdexcept>

namespace lockstep::grpc_adapter {

GrpcServer::GrpcServer(const std::string& listen_address,
                       const std::vector<grpc::Service*>& services) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials(), &port_);
    for (grpc::Service* service : services) {
        builder.RegisterService(service);
    }
    server_ = builder.BuildAndStart();
    if (!server_ || port_ == 0) {
        throw std::runtime_error("failed to bind gRPC server to " + listen_address);
    }
}

void GrpcServer::shutdown(std::chrono::milliseconds grace) {
    if (server_) {
        server_->Shutdown(std::chrono::system_clock::now() + grace);
        server_.reset();
    }
}

}  // namespace lockstep::grpc_adapter
