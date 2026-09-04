#include "veclet/node/data_node.h"

#include "temp_directory.h"
#include "veclet/index/flat_index.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace veclet::node {
namespace {

using namespace std::chrono_literals;

veclet::v1::ShardPlacement MakePlacement(uint64_t generation_id,
                                         uint64_t placement_epoch) {
  veclet::v1::ShardPlacement placement;
  placement.set_collection_id("products");
  placement.set_generation_id(generation_id);
  placement.set_shard_id(2);
  placement.set_placement_epoch(placement_epoch);
  return placement;
}

veclet::v1::VectorRecord MakeRecord(const std::string &vector_id, float first,
                                    float second) {
  veclet::v1::VectorRecord record;
  record.set_vector_id(vector_id);
  record.set_version(1);
  record.add_embedding(first);
  record.add_embedding(second);
  return record;
}

veclet::v1::SearchShardRequest
MakeSearchRequest(const veclet::v1::ShardPlacement &placement) {
  veclet::v1::SearchShardRequest request;
  *request.mutable_placement() = placement;
  request.add_query_vector(0.9F);
  request.add_query_vector(0.1F);
  request.set_k(2);
  return request;
}

veclet::v1::BatchInsertRequest
MakeBatchRequest(const veclet::v1::ShardPlacement &placement,
                 const std::vector<veclet::v1::VectorRecord> &records) {
  veclet::v1::BatchInsertRequest request;
  *request.mutable_placement() = placement;
  for (const veclet::v1::VectorRecord &record : records) {
    *request.add_records() = record;
  }
  return request;
}

class OperationGate {
public:
  void EnterAndWait() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    if (!condition_.wait_for(lock, 5s, [this] { return released_; })) {
      throw std::runtime_error("timed out waiting to release test search");
    }
  }

  bool WaitForEntry() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 5s, [this] { return entered_; });
  }

  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_{false};
  bool released_{false};
};

class InterceptingFlatIndex final : public index::LocalIndex {
public:
  explicit InterceptingFlatIndex(
      std::function<void()> before_search,
      std::function<void()> before_add = std::function<void()>{})
      : delegate_(2, index::MetricType::kL2),
        before_search_(std::move(before_search)),
        before_add_(std::move(before_add)) {}

  int dimension() const override { return delegate_.dimension(); }
  index::MetricType metric() const override { return delegate_.metric(); }
  int64_t size() const override { return delegate_.size(); }
  bool is_trained() const override { return delegate_.is_trained(); }

  void Train(std::span<const float> vectors) override {
    delegate_.Train(vectors);
  }
  void Add(std::span<const int64_t> ids,
           std::span<const float> vectors) override {
    if (before_add_) {
      before_add_();
    }
    delegate_.Add(ids, vectors);
  }
  index::SearchResult Search(std::span<const float> query,
                             int k) const override {
    if (before_search_) {
      before_search_();
    }
    return delegate_.Search(query, k);
  }
  void Remove(std::span<const int64_t> ids) override { delegate_.Remove(ids); }

private:
  index::FlatIndex delegate_;
  std::function<void()> before_search_;
  std::function<void()> before_add_;
};

class DataNodeTest : public ::testing::Test {
protected:
  void SetUp() override {
    placement_ = MakePlacement(7, 20);
    shard_ = MakeFlatShard("current", index::MetricType::kL2);
    shard_->Put(MakeRecord("v1", 1.0F, 0.0F));
    shard_->Put(MakeRecord("v2", 0.0F, 1.0F));

    service_.InstallPlacement(placement_, shard_);

    grpc::ServerBuilder builder;
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    channel_ = server_->InProcessChannel(grpc::ChannelArguments());
    stub_ = veclet::v1::DataService::NewStub(channel_);
  }

  void TearDown() override { server_->Shutdown(); }

  std::shared_ptr<shard::LocalShard> MakeFlatShard(const std::string &name,
                                                   index::MetricType metric) {
    return std::make_shared<shard::LocalShard>(
        (std::filesystem::path(temp_directory_.path()) / name).string(),
        std::make_unique<index::FlatIndex>(2, metric));
  }

  grpc::Status Search(const veclet::v1::SearchShardRequest &request,
                      veclet::v1::SearchShardResponse *response) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    return stub_->SearchShard(&context, request, response);
  }

  grpc::Status BatchInsert(const veclet::v1::BatchInsertRequest &request,
                           veclet::v1::BatchInsertResponse *response) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    return stub_->BatchInsert(&context, request, response);
  }

  testing::TempDirectory temp_directory_;
  veclet::v1::ShardPlacement placement_;
  std::shared_ptr<shard::LocalShard> shard_;
  DataNodeService service_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<veclet::v1::DataService::Stub> stub_;
};

TEST_F(DataNodeTest, SearchesInstalledPlacementAndEchoesIt) {
  veclet::v1::SearchShardResponse response;
  const grpc::Status status = Search(MakeSearchRequest(placement_), &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(response.neighbors_size(), 2);
  EXPECT_EQ(response.neighbors(0).vector_id(), "v1");
  EXPECT_EQ(response.neighbors(1).vector_id(), "v2");
  EXPECT_LT(response.neighbors(0).score(), response.neighbors(1).score());
  EXPECT_EQ(response.placement().SerializeAsString(),
            placement_.SerializeAsString());
}

TEST_F(DataNodeTest, BatchInsertsAndReportsExactRetries) {
  const std::vector<veclet::v1::VectorRecord> records = {
      MakeRecord("v3", -1.0F, 0.0F), MakeRecord("v4", 0.0F, -1.0F)};
  const veclet::v1::BatchInsertRequest request =
      MakeBatchRequest(placement_, records);

  veclet::v1::BatchInsertResponse inserted;
  const grpc::Status inserted_status = BatchInsert(request, &inserted);
  ASSERT_TRUE(inserted_status.ok()) << inserted_status.error_message();
  EXPECT_EQ(inserted.inserted_records(), 2);
  EXPECT_EQ(inserted.duplicate_records(), 0);
  EXPECT_EQ(inserted.placement().SerializeAsString(),
            placement_.SerializeAsString());
  EXPECT_TRUE(shard_->Get("v3", nullptr));
  EXPECT_TRUE(shard_->Get("v4", nullptr));
  auto search_request = MakeSearchRequest(placement_);
  search_request.set_query_vector(0, -0.9F);
  search_request.set_query_vector(1, 0.0F);
  search_request.set_k(1);
  veclet::v1::SearchShardResponse search_response;
  const grpc::Status search_status = Search(search_request, &search_response);
  ASSERT_TRUE(search_status.ok()) << search_status.error_message();
  ASSERT_EQ(search_response.neighbors_size(), 1);
  EXPECT_EQ(search_response.neighbors(0).vector_id(), "v3");

  veclet::v1::BatchInsertResponse replay;
  const grpc::Status replay_status = BatchInsert(request, &replay);
  ASSERT_TRUE(replay_status.ok()) << replay_status.error_message();
  EXPECT_EQ(replay.inserted_records(), 0);
  EXPECT_EQ(replay.duplicate_records(), 2);
  EXPECT_EQ(shard_->size(), 4);
}

TEST_F(DataNodeTest, BatchRejectsUnknownAndStalePlacementsBeforeMutation) {
  const veclet::v1::VectorRecord record =
      MakeRecord("must-not-write", -1.0F, 0.0F);
  veclet::v1::BatchInsertResponse response;

  auto invalid_collection = MakeBatchRequest(placement_, std::vector{record});
  invalid_collection.mutable_placement()->set_collection_id("Products");
  EXPECT_EQ(BatchInsert(invalid_collection, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  const auto invalid_generation =
      MakeBatchRequest(MakePlacement(0, 20), std::vector{record});
  EXPECT_EQ(BatchInsert(invalid_generation, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  const auto invalid_epoch =
      MakeBatchRequest(MakePlacement(7, 0), std::vector{record});
  EXPECT_EQ(BatchInsert(invalid_epoch, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto unknown = MakeBatchRequest(MakePlacement(7, 20), std::vector{record});
  unknown.mutable_placement()->set_collection_id("missing");
  EXPECT_EQ(BatchInsert(unknown, &response).error_code(),
            grpc::StatusCode::NOT_FOUND);

  const auto stale_generation =
      MakeBatchRequest(MakePlacement(6, 20), std::vector{record});
  EXPECT_EQ(BatchInsert(stale_generation, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  const auto stale_epoch =
      MakeBatchRequest(MakePlacement(7, 19), std::vector{record});
  EXPECT_EQ(BatchInsert(stale_epoch, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  const auto invented_future_epoch =
      MakeBatchRequest(MakePlacement(7, 21), std::vector{record});
  EXPECT_EQ(BatchInsert(invented_future_epoch, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_FALSE(shard_->Get("must-not-write", nullptr));
}

TEST_F(DataNodeTest, BatchRejectsInvalidRecordsBeforeMutation) {
  veclet::v1::BatchInsertResponse response;

  veclet::v1::BatchInsertRequest missing_placement;
  *missing_placement.add_records() =
      MakeRecord("missing-placement", 1.0F, 0.0F);
  EXPECT_EQ(BatchInsert(missing_placement, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  const std::vector<veclet::v1::VectorRecord> no_records;
  EXPECT_EQ(BatchInsert(MakeBatchRequest(placement_, no_records), &response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  const std::vector<veclet::v1::VectorRecord> duplicate_ids = {
      MakeRecord("duplicate", 1.0F, 0.0F), MakeRecord("duplicate", 0.0F, 1.0F)};
  EXPECT_EQ(BatchInsert(MakeBatchRequest(placement_, duplicate_ids), &response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  veclet::v1::VectorRecord invalid_version =
      MakeRecord("invalid-version", 1.0F, 0.0F);
  invalid_version.set_version(0);
  EXPECT_EQ(
      BatchInsert(MakeBatchRequest(placement_, {invalid_version}), &response)
          .error_code(),
      grpc::StatusCode::INVALID_ARGUMENT);

  veclet::v1::VectorRecord wrong_dimension =
      MakeRecord("wrong-dimension", 1.0F, 0.0F);
  wrong_dimension.mutable_embedding()->RemoveLast();
  EXPECT_EQ(
      BatchInsert(MakeBatchRequest(placement_, {wrong_dimension}), &response)
          .error_code(),
      grpc::StatusCode::INVALID_ARGUMENT);

  veclet::v1::VectorRecord non_finite = MakeRecord("non-finite", 1.0F, 0.0F);
  non_finite.set_embedding(1, std::numeric_limits<float>::quiet_NaN());
  EXPECT_EQ(BatchInsert(MakeBatchRequest(placement_, {non_finite}), &response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  veclet::v1::VectorRecord oversized_payload =
      MakeRecord("oversized-payload", 1.0F, 0.0F);
  oversized_payload.set_payload_data(std::string(16 * 1024 + 1, 'x'));
  EXPECT_EQ(
      BatchInsert(MakeBatchRequest(placement_, {oversized_payload}), &response)
          .error_code(),
      grpc::StatusCode::INVALID_ARGUMENT);

  EXPECT_FALSE(shard_->Get("duplicate", nullptr));
  EXPECT_FALSE(shard_->Get("invalid-version", nullptr));
  EXPECT_FALSE(shard_->Get("wrong-dimension", nullptr));
  EXPECT_FALSE(shard_->Get("non-finite", nullptr));
  EXPECT_FALSE(shard_->Get("oversized-payload", nullptr));
  EXPECT_EQ(shard_->size(), 2);
}

TEST_F(DataNodeTest, BatchMapsBoundsToResourceExhausted) {
  veclet::v1::BatchInsertResponse response;
  std::vector<veclet::v1::VectorRecord> too_many;
  too_many.reserve(257);
  for (size_t i = 0; i < 257; ++i) {
    too_many.push_back(MakeRecord("record-" + std::to_string(i), 1.0F, 0.0F));
  }
  EXPECT_EQ(BatchInsert(MakeBatchRequest(placement_, too_many), &response)
                .error_code(),
            grpc::StatusCode::RESOURCE_EXHAUSTED);

  std::vector<veclet::v1::VectorRecord> oversized_records;
  oversized_records.reserve(256);
  for (size_t i = 0; i < 256; ++i) {
    veclet::v1::VectorRecord record =
        MakeRecord("large-" + std::to_string(i), 1.0F, 0.0F);
    record.set_payload_data(std::string(16 * 1024, 'x'));
    oversized_records.push_back(std::move(record));
  }
  const veclet::v1::BatchInsertRequest oversized =
      MakeBatchRequest(placement_, oversized_records);
  ASSERT_GT(oversized.ByteSizeLong(), 4U * 1024U * 1024U);
  grpc::ServerContext context;
  EXPECT_EQ(service_.BatchInsert(&context, &oversized, &response).error_code(),
            grpc::StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_EQ(shard_->size(), 2);
}

TEST_F(DataNodeTest, BatchRejectsConflictingRecordWithoutPartialWrite) {
  veclet::v1::VectorRecord conflicting = MakeRecord("v1", 1.0F, 0.0F);
  conflicting.set_version(2);
  const veclet::v1::BatchInsertRequest request = MakeBatchRequest(
      placement_, {MakeRecord("not-written", -1.0F, 0.0F), conflicting});

  veclet::v1::BatchInsertResponse response;
  const grpc::Status status = BatchInsert(request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("collection_id=products"),
            std::string::npos);
  EXPECT_FALSE(shard_->Get("not-written", nullptr));
  EXPECT_EQ(shard_->size(), 2);
  EXPECT_FALSE(response.has_placement());
}

TEST_F(DataNodeTest, BatchRejectsZeroNormCosineRecord) {
  const veclet::v1::ShardPlacement cosine_placement = MakePlacement(7, 21);
  const auto cosine_shard =
      MakeFlatShard("cosine-insert", index::MetricType::kCosine);
  service_.InstallPlacement(cosine_placement, cosine_shard);
  const veclet::v1::BatchInsertRequest request =
      MakeBatchRequest(cosine_placement, {MakeRecord("zero", 0.0F, -0.0F)});

  veclet::v1::BatchInsertResponse response;
  EXPECT_EQ(BatchInsert(request, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(cosine_shard->Get("zero", nullptr));
}

TEST_F(DataNodeTest, RejectsMissingUnknownAndStalePlacements) {
  veclet::v1::SearchShardRequest missing;
  missing.add_query_vector(1.0F);
  missing.add_query_vector(0.0F);
  missing.set_k(1);
  veclet::v1::SearchShardResponse response;
  EXPECT_EQ(Search(missing, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto unknown = MakeSearchRequest(MakePlacement(7, 20));
  unknown.mutable_placement()->set_collection_id("missing");
  EXPECT_EQ(Search(unknown, &response).error_code(),
            grpc::StatusCode::NOT_FOUND);

  auto stale_generation = MakeSearchRequest(placement_);
  stale_generation.mutable_placement()->set_generation_id(6);
  EXPECT_EQ(Search(stale_generation, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  auto stale_epoch = MakeSearchRequest(placement_);
  stale_epoch.mutable_placement()->set_placement_epoch(19);
  EXPECT_EQ(Search(stale_epoch, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  auto invented_future_epoch = MakeSearchRequest(placement_);
  invented_future_epoch.mutable_placement()->set_placement_epoch(21);
  EXPECT_EQ(Search(invented_future_epoch, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(DataNodeTest, RejectsInvalidSearchBoundaries) {
  veclet::v1::SearchShardResponse response;

  auto invalid_generation = MakeSearchRequest(placement_);
  invalid_generation.mutable_placement()->set_generation_id(0);
  EXPECT_EQ(Search(invalid_generation, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto invalid_epoch = MakeSearchRequest(placement_);
  invalid_epoch.mutable_placement()->set_placement_epoch(0);
  EXPECT_EQ(Search(invalid_epoch, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto invalid_collection = MakeSearchRequest(placement_);
  invalid_collection.mutable_placement()->set_collection_id("Products");
  EXPECT_EQ(Search(invalid_collection, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto zero_k = MakeSearchRequest(placement_);
  zero_k.set_k(0);
  EXPECT_EQ(Search(zero_k, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto excessive_k = MakeSearchRequest(placement_);
  excessive_k.set_k(1001);
  EXPECT_EQ(Search(excessive_k, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto wrong_dimension = MakeSearchRequest(placement_);
  wrong_dimension.mutable_query_vector()->RemoveLast();
  EXPECT_EQ(Search(wrong_dimension, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto non_finite = MakeSearchRequest(placement_);
  non_finite.set_query_vector(0, std::numeric_limits<float>::infinity());
  EXPECT_EQ(Search(non_finite, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DataNodeTest, RejectsZeroNormCosineQueryOnAnEmptyShard) {
  veclet::v1::ShardPlacement cosine_placement = MakePlacement(7, 21);
  service_.InstallPlacement(
      cosine_placement, MakeFlatShard("cosine", index::MetricType::kCosine));

  auto request = MakeSearchRequest(cosine_placement);
  request.set_query_vector(0, 0.0F);
  request.set_query_vector(1, 0.0F);
  veclet::v1::SearchShardResponse response;
  EXPECT_EQ(Search(request, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DataNodeTest, RejectsOversizedRequestBeforeSearching) {
  auto request = MakeSearchRequest(placement_);
  request.mutable_query_vector()->Resize(1024 * 1024, 0.0F);

  grpc::ServerContext context;
  veclet::v1::SearchShardResponse response;
  const grpc::Status status =
      service_.SearchShard(&context, &request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
}

TEST_F(DataNodeTest, PropagatesClientCancellation) {
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 5s);
  context.TryCancel();
  veclet::v1::SearchShardResponse response;
  const grpc::Status status =
      stub_->SearchShard(&context, MakeSearchRequest(placement_), &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
}

TEST_F(DataNodeTest, BatchDoesNotMutateWhenAlreadyCancelled) {
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 5s);
  context.TryCancel();
  const veclet::v1::BatchInsertRequest request =
      MakeBatchRequest(placement_, {MakeRecord("cancelled", -1.0F, 0.0F)});
  veclet::v1::BatchInsertResponse response;

  const grpc::Status status = stub_->BatchInsert(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_FALSE(shard_->Get("cancelled", nullptr));
}

TEST_F(DataNodeTest, MapsLocalSearchFailureToInternalWithPlacementContext) {
  auto failing_index = std::make_unique<InterceptingFlatIndex>(
      [] { throw std::runtime_error("injected search failure"); });
  auto failing_shard = std::make_shared<shard::LocalShard>(
      (std::filesystem::path(temp_directory_.path()) / "failing").string(),
      std::move(failing_index));
  failing_shard->Put(MakeRecord("failing", 1.0F, 0.0F));

  const veclet::v1::ShardPlacement failing_placement = MakePlacement(7, 21);
  service_.InstallPlacement(failing_placement, failing_shard);

  veclet::v1::SearchShardResponse response;
  const grpc::Status status =
      Search(MakeSearchRequest(failing_placement), &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(status.error_message().find("collection_id=products"),
            std::string::npos);
  EXPECT_NE(status.error_message().find("placement_epoch=21"),
            std::string::npos);
  EXPECT_EQ(response.neighbors_size(), 0);
}

TEST_F(DataNodeTest, MapsLocalInsertFailureToInternalWithPlacementContext) {
  auto failing_index =
      std::make_unique<InterceptingFlatIndex>(std::function<void()>{}, [] {
        throw std::runtime_error("injected add failure");
      });
  auto failing_shard = std::make_shared<shard::LocalShard>(
      (std::filesystem::path(temp_directory_.path()) / "failing-insert")
          .string(),
      std::move(failing_index));
  const veclet::v1::ShardPlacement failing_placement = MakePlacement(7, 21);
  service_.InstallPlacement(failing_placement, failing_shard);
  const veclet::v1::BatchInsertRequest request = MakeBatchRequest(
      failing_placement, {MakeRecord("committed", 1.0F, 0.0F)});

  veclet::v1::BatchInsertResponse response;
  const grpc::Status status = BatchInsert(request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(status.error_message().find("collection_id=products"),
            std::string::npos);
  EXPECT_NE(status.error_message().find("placement_epoch=21"),
            std::string::npos);
  EXPECT_TRUE(failing_shard->Get("committed", nullptr));
  EXPECT_FALSE(response.has_placement());
}

TEST_F(DataNodeTest, DiscardsResultWhenPlacementChangesDuringSearch) {
  auto gate = std::make_shared<OperationGate>();
  auto blocking_index =
      std::make_unique<InterceptingFlatIndex>([gate] { gate->EnterAndWait(); });
  auto blocking_shard = std::make_shared<shard::LocalShard>(
      (std::filesystem::path(temp_directory_.path()) / "blocking").string(),
      std::move(blocking_index));
  blocking_shard->Put(MakeRecord("blocked", 1.0F, 0.0F));

  const veclet::v1::ShardPlacement searching_placement = MakePlacement(7, 21);
  service_.InstallPlacement(searching_placement, blocking_shard);
  const auto request = MakeSearchRequest(searching_placement);

  grpc::Status rpc_status;
  veclet::v1::SearchShardResponse response;
  std::thread client([&] { rpc_status = Search(request, &response); });

  if (!gate->WaitForEntry()) {
    gate->Release();
    client.join();
    FAIL() << "SearchShard did not reach the LocalIndex within 5 seconds";
    return;
  }

  service_.InstallPlacement(
      MakePlacement(7, 22),
      MakeFlatShard("replacement", index::MetricType::kL2));
  gate->Release();
  client.join();

  EXPECT_EQ(rpc_status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(response.neighbors_size(), 0);
  EXPECT_FALSE(response.has_placement());
}

TEST_F(DataNodeTest, DoesNotAcknowledgeInsertWhenPlacementChangesDuringWrite) {
  auto gate = std::make_shared<OperationGate>();
  auto blocking_index = std::make_unique<InterceptingFlatIndex>(
      std::function<void()>{}, [gate] { gate->EnterAndWait(); });
  auto blocking_shard = std::make_shared<shard::LocalShard>(
      (std::filesystem::path(temp_directory_.path()) / "blocking-insert")
          .string(),
      std::move(blocking_index));

  const veclet::v1::ShardPlacement writing_placement = MakePlacement(7, 21);
  service_.InstallPlacement(writing_placement, blocking_shard);
  const veclet::v1::BatchInsertRequest request = MakeBatchRequest(
      writing_placement, {MakeRecord("durable-old-placement", 1.0F, 0.0F)});

  grpc::Status rpc_status;
  veclet::v1::BatchInsertResponse response;
  std::thread client([&] { rpc_status = BatchInsert(request, &response); });

  if (!gate->WaitForEntry()) {
    gate->Release();
    client.join();
    FAIL() << "BatchInsert did not reach the LocalIndex within 5 seconds";
    return;
  }

  service_.InstallPlacement(
      MakePlacement(7, 22),
      MakeFlatShard("insert-replacement", index::MetricType::kL2));
  gate->Release();
  client.join();

  EXPECT_EQ(rpc_status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(blocking_shard->Get("durable-old-placement", nullptr));
  EXPECT_EQ(response.inserted_records(), 0);
  EXPECT_EQ(response.duplicate_records(), 0);
  EXPECT_FALSE(response.has_placement());
}

} // namespace
} // namespace veclet::node
