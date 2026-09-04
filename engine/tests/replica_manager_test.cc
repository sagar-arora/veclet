#include "veclet/node/replica_manager.h"

#include "temp_directory.h"
#include "veclet/artifact/filesystem_replica_loader.h"
#include "veclet/index/flat_index.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
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

veclet::v1::VectorRecord MakeRecord(std::string vector_id, float x, float y) {
  veclet::v1::VectorRecord record;
  record.set_vector_id(std::move(vector_id));
  record.set_version(1);
  record.add_embedding(x);
  record.add_embedding(y);
  return record;
}

artifact::ReplicaLoadRequest MakeFakeRequest(uint64_t generation_id,
                                             uint64_t placement_epoch,
                                             char manifest_character) {
  artifact::ReplicaLoadRequest request;
  request.placement = MakePlacement(generation_id, placement_epoch);
  request.manifest.manifest_id = std::string(64, manifest_character);
  request.manifest.collection_id = "products";
  request.manifest.generation_id = generation_id;
  request.manifest.shard_id = 2;
  request.manifest.dimension = 2;
  request.manifest.metric = index::MetricType::kL2;
  request.manifest.files = {
      {.relative_path = "rocksdb/CURRENT",
       .size_bytes = 0,
       .sha256 = std::string(64, '0')},
  };
  request.artifact_relative_directory = "unused";
  return request;
}

grpc::Status Search(DataNodeService *service,
                    const veclet::v1::ShardPlacement &placement,
                    veclet::v1::SearchShardResponse *response) {
  veclet::v1::SearchShardRequest request;
  *request.mutable_placement() = placement;
  request.add_query_vector(1.0F);
  request.add_query_vector(0.0F);
  request.set_k(10);
  grpc::ServerContext context;
  return service->SearchShard(&context, &request, response);
}

void CreateSourceArtifact(const std::filesystem::path &artifact_directory) {
  std::filesystem::create_directories(artifact_directory);
  {
    auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
    shard::LocalShard shard((artifact_directory / "rocksdb").string(),
                            std::move(index));
    shard.Put(MakeRecord("v1", 1.0F, 0.0F));
    shard.Put(MakeRecord("v2", 0.0F, 1.0F));
  }
}

artifact::ReplicaLoadRequest
BuildFilesystemRequest(const std::filesystem::path &source_root,
                       const std::filesystem::path &artifact_directory,
                       char manifest_character = 'a') {
  artifact::ReplicaLoadRequest request =
      MakeFakeRequest(7, 20, manifest_character);
  request.artifact_relative_directory =
      std::filesystem::relative(artifact_directory, source_root)
          .generic_string();
  request.manifest.files.clear();
  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(artifact_directory / "rocksdb")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    artifact::ArtifactFile file;
    file.relative_path =
        std::filesystem::relative(entry.path(), artifact_directory)
            .generic_string();
    file.size_bytes = entry.file_size();
    file.sha256 = artifact::ComputeFileSha256(entry.path());
    request.manifest.files.push_back(std::move(file));
  }
  std::sort(
      request.manifest.files.begin(), request.manifest.files.end(),
      [](const artifact::ArtifactFile &lhs, const artifact::ArtifactFile &rhs) {
        return lhs.relative_path < rhs.relative_path;
      });
  return request;
}

std::filesystem::path
LiveDirectory(const std::filesystem::path &data_root,
              const artifact::ReplicaLoadRequest &request) {
  return data_root / "replicas" / request.manifest.collection_id /
         ("generation-" + std::to_string(request.manifest.generation_id)) /
         ("shard-" + std::to_string(request.manifest.shard_id)) /
         ("dimension-" + std::to_string(request.manifest.dimension)) /
         "metric-l2" / ("manifest-" + request.manifest.manifest_id);
}

std::filesystem::path
CacheDirectory(const std::filesystem::path &data_root,
               const artifact::ReplicaLoadRequest &request) {
  return data_root / "artifact-cache" / request.manifest.collection_id /
         ("generation-" + std::to_string(request.manifest.generation_id)) /
         ("shard-" + std::to_string(request.manifest.shard_id)) /
         ("manifest-" + request.manifest.manifest_id);
}

void CorruptFirstNonEmptyFile(const std::filesystem::path &artifact_directory,
                              const artifact::ReplicaLoadRequest &request) {
  const auto file =
      std::find_if(request.manifest.files.begin(), request.manifest.files.end(),
                   [](const artifact::ArtifactFile &candidate) {
                     return candidate.size_bytes > 0;
                   });
  ASSERT_NE(file, request.manifest.files.end());
  const std::filesystem::path path = artifact_directory / file->relative_path;
  std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(stream.is_open());
  char byte = 0;
  stream.read(&byte, 1);
  ASSERT_TRUE(stream);
  byte ^= static_cast<char>(0xff);
  stream.seekp(0);
  stream.write(&byte, 1);
  stream.flush();
  ASSERT_TRUE(stream);
}

bool DirectoryIsAbsentOrEmpty(const std::filesystem::path &path) {
  return !std::filesystem::exists(path) ||
         std::filesystem::directory_iterator(path) ==
             std::filesystem::directory_iterator();
}

class ControlledReplicaLoader final : public artifact::ReplicaLoader {
public:
  explicit ControlledReplicaLoader(std::filesystem::path data_root)
      : data_root_(std::move(data_root)) {}

  std::shared_ptr<shard::LocalShard>
  Load(const artifact::ReplicaLoadRequest &request,
       std::stop_token stop_token) override {
    int call_number = 0;
    {
      std::unique_lock lock(mutex_);
      call_number = ++call_count_;
      if (blocked_epoch_ &&
          request.placement.placement_epoch() == *blocked_epoch_) {
        blocked_call_entered_ = true;
        condition_.notify_all();
        std::stop_callback notify_on_stop(stop_token,
                                          [this] { condition_.notify_all(); });
        condition_.wait(lock, [this, stop_token] {
          return release_blocked_call_ || stop_token.stop_requested();
        });
      }
      if (stop_token.stop_requested()) {
        throw artifact::ReplicaLoadCancelled();
      }
      if (failed_epoch_ &&
          request.placement.placement_epoch() == *failed_epoch_) {
        throw std::runtime_error("injected artifact load failure");
      }
    }

    const std::filesystem::path path =
        data_root_ /
        ("generation-" + std::to_string(request.placement.generation_id()) +
         "-epoch-" + std::to_string(request.placement.placement_epoch()) +
         "-call-" + std::to_string(call_number));
    return std::make_shared<shard::LocalShard>(
        path.string(),
        std::make_unique<index::FlatIndex>(2, index::MetricType::kL2));
  }

  void BlockEpoch(uint64_t placement_epoch) {
    std::lock_guard lock(mutex_);
    blocked_epoch_ = placement_epoch;
  }

  void FailEpoch(uint64_t placement_epoch) {
    std::lock_guard lock(mutex_);
    failed_epoch_ = placement_epoch;
  }

  bool WaitForBlockedCall() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 5s,
                               [this] { return blocked_call_entered_; });
  }

  int call_count() const {
    std::lock_guard lock(mutex_);
    return call_count_;
  }

private:
  std::filesystem::path data_root_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<uint64_t> blocked_epoch_;
  std::optional<uint64_t> failed_epoch_;
  int call_count_{0};
  bool blocked_call_entered_{false};
  bool release_blocked_call_{false};
};

TEST(ReplicaArtifactTest, RejectsInvalidIdentityPathsChecksumsAndOrdering) {
  artifact::ReplicaLoadRequest request = MakeFakeRequest(7, 20, 'a');

  auto invalid = request;
  invalid.manifest.generation_id = 8;
  EXPECT_THROW(artifact::ValidateReplicaLoadRequest(invalid),
               std::invalid_argument);

  invalid = request;
  invalid.manifest.manifest_id = std::string(64, 'A');
  EXPECT_THROW(artifact::ValidateReplicaLoadRequest(invalid),
               std::invalid_argument);

  invalid = request;
  invalid.manifest.files[0].relative_path = "../CURRENT";
  EXPECT_THROW(artifact::ValidateReplicaLoadRequest(invalid),
               std::invalid_argument);

  invalid = request;
  invalid.manifest.files.push_back(invalid.manifest.files.front());
  EXPECT_THROW(artifact::ValidateReplicaLoadRequest(invalid),
               std::invalid_argument);

  invalid = request;
  invalid.manifest.files[0].sha256 = "not-a-checksum";
  EXPECT_THROW(artifact::ValidateReplicaLoadRequest(invalid),
               std::invalid_argument);
}

TEST(FilesystemReplicaLoaderTest, LoadsVerifiedArtifactAndReusesCache) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path root = temp_directory.path();
  const std::filesystem::path source_root = root / "source";
  const std::filesystem::path artifact_directory = source_root / "artifact-7";
  const std::filesystem::path data_root = root / "data";
  CreateSourceArtifact(artifact_directory);
  const artifact::ReplicaLoadRequest request =
      BuildFilesystemRequest(source_root, artifact_directory);
  artifact::FilesystemReplicaLoader loader(source_root, data_root);

  std::shared_ptr<shard::LocalShard> loaded =
      loader.Load(request, std::stop_token{});
  ASSERT_EQ(loaded->size(), 2);
  ASSERT_EQ(loaded->Search(std::vector<float>{1.0F, 0.0F}, 1).hits.size(), 1);
  EXPECT_EQ(loaded->Search(std::vector<float>{1.0F, 0.0F}, 1)
                .hits.front()
                .record.vector_id(),
            "v1");

  loaded.reset();
  std::filesystem::remove_all(artifact_directory);
  std::filesystem::remove_all(LiveDirectory(data_root, request));

  loaded = loader.Load(request, std::stop_token{});
  EXPECT_EQ(loaded->size(), 2);
  EXPECT_EQ(loaded->Search(std::vector<float>{0.0F, 1.0F}, 1)
                .hits.front()
                .record.vector_id(),
            "v2");
  EXPECT_TRUE(DirectoryIsAbsentOrEmpty(data_root / "staging"));
}

TEST(FilesystemReplicaLoaderTest, RejectsCorruptionAndCleansStaging) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path root = temp_directory.path();
  const std::filesystem::path source_root = root / "source";
  const std::filesystem::path artifact_directory = source_root / "artifact-7";
  const std::filesystem::path data_root = root / "data";
  CreateSourceArtifact(artifact_directory);
  const artifact::ReplicaLoadRequest request =
      BuildFilesystemRequest(source_root, artifact_directory);
  CorruptFirstNonEmptyFile(artifact_directory, request);
  artifact::FilesystemReplicaLoader loader(source_root, data_root);

  EXPECT_THROW(loader.Load(request, std::stop_token{}), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(LiveDirectory(data_root, request)));
  EXPECT_TRUE(DirectoryIsAbsentOrEmpty(data_root / "staging"));
}

TEST(FilesystemReplicaLoaderTest, DiscardsAbandonedStagingOnRestart) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path root = temp_directory.path();
  const std::filesystem::path source_root = root / "source";
  const std::filesystem::path data_root = root / "data";
  std::filesystem::create_directories(source_root);
  std::filesystem::create_directories(data_root / "staging" / "abandoned");
  {
    std::ofstream partial(data_root / "staging" / "abandoned" / "partial");
    ASSERT_TRUE(partial.is_open());
    partial << "incomplete";
  }

  artifact::FilesystemReplicaLoader loader(source_root, data_root);

  EXPECT_TRUE(DirectoryIsAbsentOrEmpty(data_root / "staging"));
}

TEST(FilesystemReplicaLoaderTest, ReplacesCorruptCacheFromValidSource) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path root = temp_directory.path();
  const std::filesystem::path source_root = root / "source";
  const std::filesystem::path artifact_directory = source_root / "artifact-7";
  const std::filesystem::path data_root = root / "data";
  CreateSourceArtifact(artifact_directory);
  const artifact::ReplicaLoadRequest request =
      BuildFilesystemRequest(source_root, artifact_directory);
  artifact::FilesystemReplicaLoader loader(source_root, data_root);
  std::shared_ptr<shard::LocalShard> loaded =
      loader.Load(request, std::stop_token{});
  loaded.reset();
  std::filesystem::remove_all(LiveDirectory(data_root, request));
  CorruptFirstNonEmptyFile(CacheDirectory(data_root, request), request);

  loaded = loader.Load(request, std::stop_token{});

  EXPECT_EQ(loaded->size(), 2);
  EXPECT_EQ(loaded->Search(std::vector<float>{1.0F, 0.0F}, 1)
                .hits.front()
                .record.vector_id(),
            "v1");
  EXPECT_TRUE(DirectoryIsAbsentOrEmpty(data_root / "staging"));
}

TEST(FilesystemReplicaLoaderTest, RejectsEscapingSourceAndBoundViolations) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path root = temp_directory.path();
  const std::filesystem::path source_root = root / "source";
  const std::filesystem::path artifact_directory = source_root / "artifact-7";
  const std::filesystem::path data_root = root / "data";
  CreateSourceArtifact(artifact_directory);
  artifact::ReplicaLoadRequest request =
      BuildFilesystemRequest(source_root, artifact_directory);
  artifact::FilesystemReplicaLoader loader(source_root, data_root);

  request.artifact_relative_directory = "../artifact-7";
  EXPECT_THROW(loader.Load(request, std::stop_token{}), std::invalid_argument);

  request = BuildFilesystemRequest(source_root, artifact_directory);
  artifact::FilesystemReplicaLoaderLimits limits;
  limits.max_total_bytes = 1;
  limits.max_file_bytes = 1;
  artifact::FilesystemReplicaLoader bounded_loader(source_root, root / "small",
                                                   limits);
  EXPECT_THROW(bounded_loader.Load(request, std::stop_token{}),
               std::invalid_argument);
}

TEST(FilesystemReplicaLoaderTest,
     RejectsOverlappingRootsBeforeCreatingDataDirectory) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path source_root =
      std::filesystem::path(temp_directory.path()) / "source";
  const std::filesystem::path overlapping_data_root = source_root / "data";
  std::filesystem::create_directories(source_root);

  EXPECT_THROW(
      artifact::FilesystemReplicaLoader(source_root, overlapping_data_root),
      std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(overlapping_data_root));
}

TEST(ReplicaManagerTest, MakesFilesystemArtifactVisibleAndRemovesExactly) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path root = temp_directory.path();
  const std::filesystem::path source_root = root / "source";
  const std::filesystem::path artifact_directory = source_root / "artifact-7";
  CreateSourceArtifact(artifact_directory);
  const artifact::ReplicaLoadRequest request =
      BuildFilesystemRequest(source_root, artifact_directory);

  DataNodeService service;
  auto loader = std::make_unique<artifact::FilesystemReplicaLoader>(
      source_root, root / "data");
  ReplicaManager manager(service, std::move(loader));

  EXPECT_EQ(manager.PreparePlacement(request), PreparePlacementOutcome::kReady);
  EXPECT_EQ(manager.PreparePlacement(request),
            PreparePlacementOutcome::kAlreadyReady);
  const std::optional<ReplicaStatus> status = manager.Status("products", 2);
  ASSERT_TRUE(status);
  EXPECT_EQ(status->state, ReplicaState::kReady);
  ASSERT_TRUE(status->active_placement);
  EXPECT_EQ(status->active_placement->placement_epoch(), 20);

  veclet::v1::SearchShardResponse response;
  grpc::Status search_status = Search(&service, request.placement, &response);
  ASSERT_TRUE(search_status.ok()) << search_status.error_message();
  ASSERT_EQ(response.neighbors_size(), 2);
  EXPECT_EQ(response.neighbors(0).vector_id(), "v1");

  EXPECT_FALSE(manager.RemovePlacement(MakePlacement(7, 19)));
  EXPECT_TRUE(manager.RemovePlacement(request.placement));
  EXPECT_FALSE(manager.Status("products", 2));
  response.Clear();
  search_status = Search(&service, request.placement, &response);
  EXPECT_EQ(search_status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST(ReplicaManagerTest, NewerPlacementSupersedesBlockedRecovery) {
  testing::TempDirectory temp_directory;
  DataNodeService service;
  auto loader =
      std::make_unique<ControlledReplicaLoader>(temp_directory.path());
  ControlledReplicaLoader *loader_view = loader.get();
  loader_view->BlockEpoch(20);
  ReplicaManager manager(service, std::move(loader));
  const artifact::ReplicaLoadRequest older = MakeFakeRequest(7, 20, 'a');
  const artifact::ReplicaLoadRequest newer = MakeFakeRequest(8, 21, 'b');
  std::optional<PreparePlacementOutcome> older_outcome;
  std::exception_ptr older_error;

  std::thread older_thread([&] {
    try {
      older_outcome = manager.PreparePlacement(older);
    } catch (...) {
      older_error = std::current_exception();
    }
  });
  if (!loader_view->WaitForBlockedCall()) {
    static_cast<void>(manager.RemovePlacement(older.placement));
    older_thread.join();
    FAIL() << "timed out waiting for blocked replica preparation";
  }

  EXPECT_EQ(manager.PreparePlacement(newer), PreparePlacementOutcome::kReady);
  older_thread.join();
  ASSERT_FALSE(older_error);
  ASSERT_TRUE(older_outcome);
  EXPECT_EQ(*older_outcome, PreparePlacementOutcome::kSuperseded);

  const std::optional<ReplicaStatus> status = manager.Status("products", 2);
  ASSERT_TRUE(status);
  EXPECT_EQ(status->state, ReplicaState::kReady);
  EXPECT_EQ(status->desired_placement.generation_id(), 8);
  ASSERT_TRUE(status->active_placement);
  EXPECT_EQ(status->active_placement->placement_epoch(), 21);

  veclet::v1::SearchShardResponse response;
  EXPECT_EQ(Search(&service, older.placement, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(Search(&service, newer.placement, &response).ok());
}

TEST(ReplicaManagerTest, ExactRemoveCancelsBlockedRecovery) {
  testing::TempDirectory temp_directory;
  DataNodeService service;
  auto loader =
      std::make_unique<ControlledReplicaLoader>(temp_directory.path());
  ControlledReplicaLoader *loader_view = loader.get();
  loader_view->BlockEpoch(20);
  ReplicaManager manager(service, std::move(loader));
  const artifact::ReplicaLoadRequest request = MakeFakeRequest(7, 20, 'a');
  std::optional<PreparePlacementOutcome> outcome;
  std::exception_ptr error;

  std::thread preparation([&] {
    try {
      outcome = manager.PreparePlacement(request);
    } catch (...) {
      error = std::current_exception();
    }
  });
  if (!loader_view->WaitForBlockedCall()) {
    static_cast<void>(manager.RemovePlacement(request.placement));
    preparation.join();
    FAIL() << "timed out waiting for blocked replica preparation";
  }
  EXPECT_TRUE(manager.RemovePlacement(request.placement));
  preparation.join();

  ASSERT_FALSE(error);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(*outcome, PreparePlacementOutcome::kSuperseded);
  EXPECT_FALSE(manager.Status("products", 2));
  veclet::v1::SearchShardResponse response;
  EXPECT_EQ(Search(&service, request.placement, &response).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

TEST(ReplicaManagerTest, CallerCancellationFailsWithoutActivation) {
  testing::TempDirectory temp_directory;
  DataNodeService service;
  auto loader =
      std::make_unique<ControlledReplicaLoader>(temp_directory.path());
  ControlledReplicaLoader *loader_view = loader.get();
  loader_view->BlockEpoch(20);
  ReplicaManager manager(service, std::move(loader));
  const artifact::ReplicaLoadRequest request = MakeFakeRequest(7, 20, 'a');
  std::stop_source cancellation;
  std::optional<PreparePlacementOutcome> outcome;
  std::exception_ptr error;

  std::thread preparation([&] {
    try {
      outcome = manager.PreparePlacement(request, cancellation.get_token());
    } catch (...) {
      error = std::current_exception();
    }
  });
  if (!loader_view->WaitForBlockedCall()) {
    cancellation.request_stop();
    preparation.join();
    FAIL() << "timed out waiting for blocked replica preparation";
  }
  cancellation.request_stop();
  preparation.join();

  ASSERT_FALSE(error);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(*outcome, PreparePlacementOutcome::kCancelled);
  const std::optional<ReplicaStatus> status = manager.Status("products", 2);
  ASSERT_TRUE(status);
  EXPECT_EQ(status->state, ReplicaState::kFailed);
  EXPECT_FALSE(status->failure.empty());
  EXPECT_FALSE(status->active_placement);
  veclet::v1::SearchShardResponse response;
  EXPECT_EQ(Search(&service, request.placement, &response).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

TEST(ReplicaManagerTest, FailedReplacementKeepsPriorPlacementActive) {
  testing::TempDirectory temp_directory;
  DataNodeService service;
  auto loader =
      std::make_unique<ControlledReplicaLoader>(temp_directory.path());
  ControlledReplicaLoader *loader_view = loader.get();
  ReplicaManager manager(service, std::move(loader));
  const artifact::ReplicaLoadRequest current = MakeFakeRequest(7, 20, 'a');
  const artifact::ReplicaLoadRequest replacement = MakeFakeRequest(8, 21, 'b');

  EXPECT_EQ(manager.PreparePlacement(current), PreparePlacementOutcome::kReady);
  loader_view->FailEpoch(21);
  EXPECT_THROW(manager.PreparePlacement(replacement), std::runtime_error);

  const std::optional<ReplicaStatus> status = manager.Status("products", 2);
  ASSERT_TRUE(status);
  EXPECT_EQ(status->state, ReplicaState::kFailed);
  EXPECT_FALSE(status->failure.empty());
  ASSERT_TRUE(status->active_placement);
  EXPECT_EQ(status->active_placement->placement_epoch(), 20);

  veclet::v1::SearchShardResponse response;
  EXPECT_TRUE(Search(&service, current.placement, &response).ok());
  EXPECT_EQ(Search(&service, replacement.placement, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

TEST(ReplicaManagerTest, RefencesReadyReplicaWithoutReloadingArtifacts) {
  testing::TempDirectory temp_directory;
  DataNodeService service;
  auto loader =
      std::make_unique<ControlledReplicaLoader>(temp_directory.path());
  ControlledReplicaLoader *loader_view = loader.get();
  ReplicaManager manager(service, std::move(loader));
  const artifact::ReplicaLoadRequest current = MakeFakeRequest(7, 20, 'a');
  artifact::ReplicaLoadRequest refenced = MakeFakeRequest(7, 21, 'a');

  EXPECT_EQ(manager.PreparePlacement(current), PreparePlacementOutcome::kReady);
  EXPECT_EQ(loader_view->call_count(), 1);
  EXPECT_EQ(manager.PreparePlacement(refenced),
            PreparePlacementOutcome::kReady);
  EXPECT_EQ(loader_view->call_count(), 1);

  veclet::v1::SearchShardResponse response;
  EXPECT_EQ(Search(&service, current.placement, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(Search(&service, refenced.placement, &response).ok());
}

TEST(ReplicaManagerTest, RejectsStaleEpochAndGenerationArtifactMutation) {
  testing::TempDirectory temp_directory;
  DataNodeService service;
  auto loader =
      std::make_unique<ControlledReplicaLoader>(temp_directory.path());
  ReplicaManager manager(service, std::move(loader));
  const artifact::ReplicaLoadRequest current = MakeFakeRequest(7, 20, 'a');
  EXPECT_EQ(manager.PreparePlacement(current), PreparePlacementOutcome::kReady);

  EXPECT_THROW(manager.PreparePlacement(MakeFakeRequest(7, 19, 'a')),
               std::domain_error);
  EXPECT_THROW(manager.PreparePlacement(MakeFakeRequest(7, 21, 'b')),
               std::domain_error);
  EXPECT_THROW(manager.PreparePlacement(MakeFakeRequest(6, 21, 'c')),
               std::domain_error);
}

} // namespace
} // namespace veclet::node
