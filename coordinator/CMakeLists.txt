add_executable(coordinator main.cpp)
target_link_libraries(coordinator PRIVATE distbuild_proto gRPC::grpc++)
