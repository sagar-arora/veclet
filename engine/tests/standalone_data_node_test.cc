#include "veclet/node/standalone_data_node.h"

#include "temp_directory.h"
#include "veclet/v1/data.grpc.pb.h"

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/slice.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace veclet::node {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kHealthCheckMethod = "/grpc.health.v1.Health/Check";

StandaloneDataNodeConfig MakeConfig(const std::string &data_directory) {
  StandaloneDataNodeConfig config;
  config.listen_address = "127.0.0.1:0";
  config.data_directory = data_directory;
  config.collection_id = "products";
  config.shard_id = 2;
  config.generation_id = 7;
  config.placement_epoch = 20;
  config.dimension = 2;
  config.metric = index::MetricType::kL2;
  return config;
}

veclet::v1::ShardPlacement MakePlacement(uint64_t generation_id = 7,
                                         uint64_t placement_epoch = 20) {
  veclet::v1::ShardPlacement placement;
  placement.set_collection_id("products");
  placement.set_shard_id(2);
  placement.set_generation_id(generation_id);
  placement.set_placement_epoch(placement_epoch);
  return placement;
}

veclet::v1::VectorRecord MakeRecord(std::string vector_id, float x, float y) {
  veclet::v1::VectorRecord record;
  record.set_vector_id(std::move(vector_id));
  record.set_version(1);
  record.add_embedding(x);
  record.add_embedding(y);
  return record;
}

grpc::Status CheckHealth(const std::shared_ptr<grpc::Channel> &channel,
                         std::string_view service_name, int *serving_status) {
  std::string encoded_request;
  if (!service_name.empty()) {
    if (service_name.size() > 127) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "test health service name is too long");
    }
    encoded_request.push_back(static_cast<char>(0x0a));
    encoded_request.push_back(static_cast<char>(service_name.size()));
    encoded_request.append(service_name);
  }
  const grpc::Slice request_slice(encoded_request);
  const grpc::ByteBuffer request(&request_slice, 1);

  grpc::GenericStub stub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 5s);
  grpc::CompletionQueue queue;
  grpc::ByteBuffer response;
  grpc::Status status;
  std::unique_ptr<grpc::GenericClientAsyncResponseReader> call =
      stub.PrepareUnaryCall(&context, std::string(kHealthCheckMethod), request,
                            &queue);
  if (!call) {
    queue.Shutdown();
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "failed to prepare health RPC");
  }

  call->StartCall();
  int completion_tag = 0;
  void *const expected_tag = &completion_tag;
  call->Finish(&response, &status, expected_tag);
  void *received_tag = nullptr;
  bool ok = false;
  const bool received = queue.Next(&received_tag, &ok);
  queue.Shutdown();
  void *ignored_tag = nullptr;
  bool ignored_ok = false;
  while (queue.Next(&ignored_tag, &ignored_ok)) {
  }
  if (!received || !ok || received_tag != expected_tag) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "health RPC completion was invalid");
  }
  if (!status.ok()) {
    return status;
  }

  grpc::Slice response_slice;
  const grpc::Status flatten_status =
      response.DumpToSingleSlice(&response_slice);
  if (!flatten_status.ok()) {
    return flatten_status;
  }
  if (response_slice.size() != 2 || response_slice.begin()[0] != 0x08) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "health response has unexpected encoding");
  }
  *serving_status = response_slice.begin()[1];
  return grpc::Status::OK;
}

TEST(StandaloneDataNodeConfigTest, ParsesRequiredFlagsAndDefaults) {
  const std::vector<std::string_view> arguments = {
      "--data-directory=/tmp/veclet-products",
      "--collection-id=products-2026",
      "--dimension=384",
      "--metric=cosine",
  };

  const StandaloneDataNodeConfig config =
      ParseStandaloneDataNodeArgs(arguments);

  EXPECT_EQ(config.listen_address, "127.0.0.1:7402");
  EXPECT_EQ(config.data_directory, "/tmp/veclet-products");
  EXPECT_EQ(config.collection_id, "products-2026");
  EXPECT_EQ(config.shard_id, 0);
  EXPECT_EQ(config.generation_id, 1);
  EXPECT_EQ(config.placement_epoch, 1);
  EXPECT_EQ(config.dimension, 384);
  EXPECT_EQ(config.metric, index::MetricType::kCosine);
  EXPECT_EQ(config.hnsw_m, 32);
  EXPECT_EQ(config.hnsw_ef_search, 64);
}

TEST(StandaloneDataNodeConfigTest, ParsesEveryOverride) {
  const std::vector<std::string_view> arguments = {
      "--data-directory=/tmp/veclet-products",
      "--collection-id=products",
      "--dimension=128",
      "--metric=inner-product",
      "--listen-address=localhost:9000",
      "--shard-id=4294967295",
      "--generation-id=9",
      "--placement-epoch=12",
      "--index=hnsw",
      "--hnsw-m=48",
      "--hnsw-ef-search=100",
  };

  const StandaloneDataNodeConfig config =
      ParseStandaloneDataNodeArgs(arguments);

  EXPECT_EQ(config.listen_address, "localhost:9000");
  EXPECT_EQ(config.shard_id, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(config.generation_id, 9);
  EXPECT_EQ(config.placement_epoch, 12);
  EXPECT_EQ(config.dimension, 128);
  EXPECT_EQ(config.metric, index::MetricType::kInnerProduct);
  EXPECT_EQ(config.hnsw_m, 48);
  EXPECT_EQ(config.hnsw_ef_search, 100);
}

TEST(StandaloneDataNodeConfigTest, RejectsInvalidAndUnsafeArguments) {
  const auto reject = [](std::vector<std::string_view> arguments) {
    EXPECT_THROW(ParseStandaloneDataNodeArgs(arguments), std::invalid_argument);
  };
  const std::vector<std::string_view> required = {
      "--data-directory=/tmp/veclet-products",
      "--collection-id=products",
      "--dimension=2",
      "--metric=l2",
  };

  reject({required[0], required[1], required[2]});
  reject(
      {required[0], required[1], required[2], required[3], "--metric=cosine"});
  reject(
      {required[0], required[1], required[2], required[3], "--unknown=value"});
  reject({required[0], required[1], required[2], required[3], "positional"});
  reject({"--data-directory=", required[1], required[2], required[3]});
  reject({required[0], "--collection-id=Products", required[2], required[3]});
  reject({required[0], required[1], "--dimension=0", required[3]});
  reject({required[0], required[1], required[2], "--metric=euclidean"});
  reject({required[0], required[1], required[2], required[3], "--index=ivf"});
  reject({required[0], required[1], required[2], required[3],
          "--generation-id=0"});
  reject({required[0], required[1], required[2], required[3],
          "--placement-epoch=0"});
  reject(
      {required[0], required[1], required[2], required[3], "--hnsw-m=65537"});
  reject({required[0], required[1], required[2], required[3],
          "--hnsw-ef-search=-1"});
  reject({required[0], required[1], required[2], required[3],
          "--listen-address=0.0.0.0:7402"});
  reject({required[0], required[1], required[2], required[3],
          "--listen-address=127.0.0.1:0"});
  reject({required[0], required[1], required[2], required[3],
          "--listen-address=localhost:65536"});
}

TEST(StandaloneDataNodeConfigTest, InvalidConfigDoesNotOpenRocksDb) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path database_path =
      std::filesystem::path(temp_directory.path()) / "must-not-exist";
  StandaloneDataNodeConfig config = MakeConfig(database_path.string());
  config.collection_id = "INVALID";

  EXPECT_THROW(StandaloneDataNode(std::move(config), true),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(database_path));
}

TEST(StandaloneDataNodeTest, ServesHealthRpcAndRecoversAcrossRestart) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path storage_root =
      std::filesystem::path(temp_directory.path()) / "storage";

  {
    StandaloneDataNode data_node(MakeConfig(storage_root.string()), true);
    data_node.Start();
    EXPECT_GT(data_node.selected_port(), 0);
    EXPECT_EQ(data_node.endpoint(),
              "127.0.0.1:" + std::to_string(data_node.selected_port()));

    const std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
        data_node.endpoint(), grpc::InsecureChannelCredentials());
    ASSERT_TRUE(
        channel->WaitForConnected(std::chrono::system_clock::now() + 5s));

    int serving_status = 0;
    grpc::Status health_status = CheckHealth(channel, "", &serving_status);
    ASSERT_TRUE(health_status.ok()) << health_status.error_message();
    EXPECT_EQ(serving_status, 1);
    health_status =
        CheckHealth(channel, "veclet.v1.DataService", &serving_status);
    ASSERT_TRUE(health_status.ok()) << health_status.error_message();
    EXPECT_EQ(serving_status, 1);

    std::unique_ptr<veclet::v1::DataService::Stub> stub =
        veclet::v1::DataService::NewStub(channel);
    veclet::v1::BatchInsertRequest insert_request;
    *insert_request.mutable_placement() = MakePlacement();
    *insert_request.add_records() = MakeRecord("v1", 1.0F, 0.0F);
    *insert_request.add_records() = MakeRecord("v2", 0.0F, 1.0F);
    grpc::ClientContext insert_context;
    insert_context.set_deadline(std::chrono::system_clock::now() + 5s);
    veclet::v1::BatchInsertResponse insert_response;
    const grpc::Status insert_status =
        stub->BatchInsert(&insert_context, insert_request, &insert_response);
    ASSERT_TRUE(insert_status.ok()) << insert_status.error_message();
    EXPECT_EQ(insert_response.inserted_records(), 2);
    EXPECT_EQ(insert_response.duplicate_records(), 0);

    veclet::v1::SearchShardRequest search_request;
    *search_request.mutable_placement() = MakePlacement();
    search_request.add_query_vector(0.9F);
    search_request.add_query_vector(0.1F);
    search_request.set_k(1);
    grpc::ClientContext search_context;
    search_context.set_deadline(std::chrono::system_clock::now() + 5s);
    veclet::v1::SearchShardResponse search_response;
    const grpc::Status search_status =
        stub->SearchShard(&search_context, search_request, &search_response);
    ASSERT_TRUE(search_status.ok()) << search_status.error_message();
    ASSERT_EQ(search_response.neighbors_size(), 1);
    EXPECT_EQ(search_response.neighbors(0).vector_id(), "v1");

    EXPECT_THROW(data_node.Start(), std::logic_error);
    data_node.Shutdown();
    data_node.Shutdown();
    data_node.Wait();
  }

  {
    StandaloneDataNode recovered(MakeConfig(storage_root.string()), true);
    recovered.Start();
    const std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
        recovered.endpoint(), grpc::InsecureChannelCredentials());
    ASSERT_TRUE(
        channel->WaitForConnected(std::chrono::system_clock::now() + 5s));

    int serving_status = 0;
    const grpc::Status health_status =
        CheckHealth(channel, "veclet.v1.DataService", &serving_status);
    ASSERT_TRUE(health_status.ok()) << health_status.error_message();
    EXPECT_EQ(serving_status, 1);

    std::unique_ptr<veclet::v1::DataService::Stub> stub =
        veclet::v1::DataService::NewStub(channel);
    veclet::v1::SearchShardRequest search_request;
    *search_request.mutable_placement() = MakePlacement();
    search_request.add_query_vector(0.0F);
    search_request.add_query_vector(0.9F);
    search_request.set_k(1);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    veclet::v1::SearchShardResponse response;
    const grpc::Status status =
        stub->SearchShard(&context, search_request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(response.neighbors_size(), 1);
    EXPECT_EQ(response.neighbors(0).vector_id(), "v2");

    recovered.Shutdown();
    recovered.Wait();
  }

  {
    StandaloneDataNodeConfig next_generation =
        MakeConfig(storage_root.string());
    next_generation.generation_id = 8;
    next_generation.placement_epoch = 21;
    StandaloneDataNode isolated(std::move(next_generation), true);
    isolated.Start();
    const std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
        isolated.endpoint(), grpc::InsecureChannelCredentials());
    ASSERT_TRUE(
        channel->WaitForConnected(std::chrono::system_clock::now() + 5s));

    std::unique_ptr<veclet::v1::DataService::Stub> stub =
        veclet::v1::DataService::NewStub(channel);
    veclet::v1::SearchShardRequest search_request;
    *search_request.mutable_placement() = MakePlacement(8, 21);
    search_request.add_query_vector(1.0F);
    search_request.add_query_vector(0.0F);
    search_request.set_k(1);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    veclet::v1::SearchShardResponse response;
    const grpc::Status status =
        stub->SearchShard(&context, search_request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.neighbors_size(), 0);

    isolated.Shutdown();
    isolated.Wait();
  }
}

} // namespace
} // namespace veclet::node
