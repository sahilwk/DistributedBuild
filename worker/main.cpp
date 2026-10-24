#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "worker_service.grpc.pb.h"

namespace {

// WorkerService is the only thing crossing the wire here: the RPC boundary
// is Ping(PingRequest) -> PingResponse. Everything else (the worker's own
// id, how it logs) stays local to this process and is never serialized.
class WorkerServiceImpl final : public distbuild::workersvc::WorkerService::Service {
 public:
  explicit WorkerServiceImpl(std::string worker_id) : worker_id_(std::move(worker_id)) {}

  grpc::Status Ping(grpc::ServerContext* /*context*/, const distbuild::workersvc::PingRequest* request,
                     distbuild::workersvc::PingResponse* response) override {
    std::cout << "[worker] ping from '" << request->caller_id() << "'" << std::endl;
    response->set_worker_id(worker_id_);
    return grpc::Status::OK;
  }

 private:
  std::string worker_id_;
};

}  // namespace

int main(int argc, char** argv) {
  std::string listen_address = "0.0.0.0:50052";
  if (argc > 1) {
    listen_address = argv[1];
  }

  WorkerServiceImpl service("worker-1");

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "[worker] failed to start on " << listen_address << std::endl;
    return 1;
  }

  std::cout << "[worker] WorkerService listening on " << listen_address << std::endl;
  server->Wait();

  return 0;
}
