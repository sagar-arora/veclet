set(veclet_proto_files
    veclet/storage/v1/record.proto
    veclet/v1/collection.proto
    veclet/v1/common.proto
    veclet/v1/data.proto
    veclet/v1/query.proto)

set(veclet_proto_dependencies)
set(veclet_generated_sources)
set(veclet_generated_headers)
foreach(proto_file IN LISTS veclet_proto_files)
  list(APPEND veclet_proto_dependencies
       "${PROJECT_SOURCE_DIR}/api/proto/${proto_file}")

  string(REGEX REPLACE "\\.proto$" ".pb.cc" message_source "${proto_file}")
  string(REGEX REPLACE "\\.proto$" ".pb.h" message_header "${proto_file}")
  string(REGEX REPLACE "\\.proto$" ".grpc.pb.cc" service_source "${proto_file}")
  string(REGEX REPLACE "\\.proto$" ".grpc.pb.h" service_header "${proto_file}")

  list(APPEND veclet_generated_sources
       "${CMAKE_CURRENT_BINARY_DIR}/generated/${message_source}"
       "${CMAKE_CURRENT_BINARY_DIR}/generated/${service_source}")
  list(APPEND veclet_generated_headers
       "${CMAKE_CURRENT_BINARY_DIR}/generated/${message_header}"
       "${CMAKE_CURRENT_BINARY_DIR}/generated/${service_header}")
endforeach()

add_custom_command(
  OUTPUT ${veclet_generated_sources} ${veclet_generated_headers}
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${BUF_EXECUTABLE}" generate "${PROJECT_SOURCE_DIR}"
          --template "${PROJECT_BINARY_DIR}/buf.gen.cpp.yaml"
          --output "${CMAKE_CURRENT_BINARY_DIR}/generated"
  DEPENDS
    ${veclet_proto_dependencies}
    "${PROJECT_SOURCE_DIR}/buf.yaml"
    "${PROJECT_SOURCE_DIR}/cmake/buf.gen.cpp.yaml.in"
    "${PROJECT_BINARY_DIR}/buf.gen.cpp.yaml"
  COMMENT "Generating local C++ protobuf and gRPC bindings"
  VERBATIM)

add_library(veclet_proto STATIC
            ${veclet_generated_sources}
            ${veclet_generated_headers})
target_include_directories(
  veclet_proto
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>")
target_link_libraries(veclet_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
target_compile_features(veclet_proto PUBLIC cxx_std_20)

add_executable(veclet_proto_contract_test tests/proto_contract_test.cc)
target_link_libraries(veclet_proto_contract_test
                      PRIVATE veclet_proto GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(veclet_proto_contract_test DISCOVERY_TIMEOUT 30)
