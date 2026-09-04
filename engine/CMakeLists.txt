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

find_package(OpenMP REQUIRED)
find_package(faiss CONFIG REQUIRED)
find_package(RocksDB CONFIG REQUIRED)

add_library(
  veclet_index STATIC
  src/index/flat_index.cc
  src/index/ivf_flat_index.cc
  src/index/hnsw_flat_index.cc)
target_include_directories(
  veclet_index
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(veclet_index PUBLIC faiss)
target_compile_features(veclet_index PUBLIC cxx_std_20)

add_executable(veclet_flat_index_test tests/flat_index_test.cc)
target_link_libraries(veclet_flat_index_test
                      PRIVATE veclet_index GTest::gtest_main)
gtest_discover_tests(veclet_flat_index_test DISCOVERY_TIMEOUT 30)

add_executable(veclet_ivf_flat_index_test tests/ivf_flat_index_test.cc)
target_link_libraries(veclet_ivf_flat_index_test
                      PRIVATE veclet_index GTest::gtest_main)
gtest_discover_tests(veclet_ivf_flat_index_test DISCOVERY_TIMEOUT 30)

add_executable(veclet_hnsw_flat_index_test tests/hnsw_flat_index_test.cc)
target_link_libraries(veclet_hnsw_flat_index_test
                      PRIVATE veclet_index GTest::gtest_main)
gtest_discover_tests(veclet_hnsw_flat_index_test DISCOVERY_TIMEOUT 30)

add_library(
  veclet_shard STATIC
  src/shard/rocks_store.cc
  src/shard/local_shard.cc)
target_include_directories(
  veclet_shard
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(veclet_shard
                      PUBLIC veclet_proto veclet_index RocksDB::rocksdb)
target_compile_features(veclet_shard PUBLIC cxx_std_20)

add_executable(veclet_rocks_store_test tests/rocks_store_test.cc)
target_link_libraries(veclet_rocks_store_test
                      PRIVATE veclet_shard GTest::gtest_main)
gtest_discover_tests(veclet_rocks_store_test DISCOVERY_TIMEOUT 30)

add_library(veclet_node_registry STATIC src/node/shard_registry.cc)
target_include_directories(
  veclet_node_registry
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(veclet_node_registry PUBLIC veclet_proto veclet_shard)
target_compile_features(veclet_node_registry PUBLIC cxx_std_20)

add_executable(veclet_shard_registry_test tests/shard_registry_test.cc)
target_link_libraries(veclet_shard_registry_test
                      PRIVATE veclet_node_registry GTest::gtest_main)
gtest_discover_tests(veclet_shard_registry_test DISCOVERY_TIMEOUT 30)

add_executable(veclet_local_shard_test tests/local_shard_test.cc)
target_link_libraries(veclet_local_shard_test
                      PRIVATE veclet_shard GTest::gtest_main)
gtest_discover_tests(veclet_local_shard_test DISCOVERY_TIMEOUT 30)

add_library(
  veclet_node STATIC
  src/node/data_node.cc)
target_include_directories(
  veclet_node
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(veclet_node PUBLIC veclet_node_registry gRPC::grpc++)
target_compile_features(veclet_node PUBLIC cxx_std_20)

add_executable(veclet_data_node_test tests/data_node_test.cc)
target_link_libraries(veclet_data_node_test
                      PRIVATE veclet_node GTest::gtest_main)
gtest_discover_tests(veclet_data_node_test DISCOVERY_TIMEOUT 30)
