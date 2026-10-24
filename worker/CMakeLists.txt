add_executable(worker main.cpp)
target_link_libraries(worker PRIVATE distbuild_proto gRPC::grpc++)
