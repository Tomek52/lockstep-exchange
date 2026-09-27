#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace lockstep::grpc_adapter {

/// Owns the gRPC server. Construction binds and starts it; throws
/// std::runtime_error if the address cannot be bound (startup error, ADR-0008).
class GrpcServer {
public:
    GrpcServer(const std::string& listen_address, const std::vector<grpc::Service*>& services);

    /// The bound port (useful with "host:0" in tests).
    [[nodiscard]] int port() const noexcept { return port_; }

    /// Stops accepting calls, lets in-flight calls finish until `grace`
    /// expires, then cancels the rest. Must run before Engine::stop().
    void shutdown(std::chrono::milliseconds grace);

private:
    int port_{0};
    std::unique_ptr<grpc::Server> server_;
};

}  // namespace lockstep::grpc_adapter
