#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "build_service.grpc.pb.h"
#include "worker_service.grpc.pb.h"

namespace {

// BuildService is the CLI -> coordinator boundary. No real logic yet — it
// exists on Day 1 so the coordinator binary proves it can be a gRPC server,
// not just a client, before the CLI shows up in a later phase.
class BuildServiceImpl final : public distbuild::buildsvc::BuildService::Service {
 public:
  grpc::Status Ping(grpc::ServerContext* /*context*/, const distbuild::buildsvc::PingRequest* /*request*/,
                     distbuild::buildsvc::PingResponse* response) override {
    response->set_coordinator_id("coordinator-1");
    return grpc::Status::OK;
  }
};

// The coordinator is the client for WorkerService: it is the one deciding
// which worker to call, so it owns the stub. A worker never dials the
// coordinator through this service.
bool PingWorker(const std::string& worker_address) {
  auto channel = grpc::CreateChannel(worker_address, grpc::InsecureChannelCredentials());
  std::unique_ptr<distbuild::workersvc::WorkerService::Stub> stub(
      distbuild::workersvc::WorkerService::NewStub(channel));

  distbuild::workersvc::PingRequest request;
  request.set_caller_id("coordinator-1");

  distbuild::workersvc::PingResponse response;
  grpc::ClientContext context;

  grpc::Status status = stub->Ping(&context, request, &response);
  if (!status.ok()) {
    std::cerr << "[coordinator] ping to worker failed: " << status.error_message() << std::endl;
    return false;
  }

  std::cout << "[coordinator] ping to worker ok, worker_id='" << response.worker_id() << "'"
            << std::endl;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string worker_address = "localhost:50052";
  std::string listen_address = "0.0.0.0:50051";
  if (argc > 1) {
    worker_address = argv[1];
  }
  if (argc > 2) {
    listen_address = argv[2];
  }

  BuildServiceImpl service;
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "[coordinator] failed to start on " << listen_address << std::endl;
    return 1;
  }
  std::cout << "[coordinator] BuildService listening on " << listen_address << std::endl;

  if (!PingWorker(worker_address)) {
    return 1;
  }

  server->Wait();
  return 0;
}
