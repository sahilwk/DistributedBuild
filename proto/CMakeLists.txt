# Generates and builds the .pb.cc/.grpc.pb.cc sources for every .proto file
# in this directory into a single static library other targets link against.
# Centralized here (rather than each binary generating its own copy) because
# coordinator and worker both need worker_service's generated code, and
# generating it twice would mean two divergent copies of the same symbols.

set(PROTO_FILES
    build_service.proto
    worker_service.proto
)

set(GENERATED_SRCS)
foreach(proto_file ${PROTO_FILES})
    get_filename_component(proto_name ${proto_file} NAME_WE)

    set(pb_cc "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.pb.cc")
    set(pb_h "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.pb.h")
    set(grpc_pb_cc "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.grpc.pb.cc")
    set(grpc_pb_h "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.grpc.pb.h")

    add_custom_command(
        OUTPUT ${pb_cc} ${pb_h} ${grpc_pb_cc} ${grpc_pb_h}
        COMMAND protobuf::protoc
        ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
             --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
             -I ${CMAKE_CURRENT_SOURCE_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/${proto_file}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${proto_file} protobuf::protoc gRPC::grpc_cpp_plugin
        COMMENT "Generating gRPC/Protobuf sources for ${proto_file}"
        VERBATIM
    )

    list(APPEND GENERATED_SRCS ${pb_cc} ${grpc_pb_cc})
endforeach()

add_library(distbuild_proto STATIC ${GENERATED_SRCS})
target_link_libraries(distbuild_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)
target_include_directories(distbuild_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
