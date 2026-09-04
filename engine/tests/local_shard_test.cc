#include "veclet/shard/local_shard.h"

#include "temp_directory.h"
#include "veclet/index/flat_index.h"
#include "veclet/index/ivf_flat_index.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace veclet::shard {
namespace {

veclet::v1::VectorRecord MakeRecord(std::string vector_id, float x, float y) {
  veclet::v1::VectorRecord record;
  record.set_vector_id(std::move(vector_id));
  record.set_version(1);
  record.add_embedding(x);
  record.add_embedding(y);
  record.set_payload_data("metadata");
  return record;
}

class FailFirstAddIndex final : public index::LocalIndex {
public:
  int dimension() const override { return delegate_.dimension(); }
  index::MetricType metric() const override { return delegate_.metric(); }
  int64_t size() const override { return delegate_.size(); }
  bool is_trained() const override { return delegate_.is_trained(); }

  void Train(std::span<const float> vectors) override {
    delegate_.Train(vectors);
  }

  void Add(std::span<const int64_t> ids,
           std::span<const float> vectors) override {
    if (fail_first_add_) {
      fail_first_add_ = false;
      throw std::runtime_error("injected FAISS add failure");
    }
    delegate_.Add(ids, vectors);
  }

  index::SearchResult Search(std::span<const float> query,
                             int k) const override {
    return delegate_.Search(query, k);
  }

  void Remove(std::span<const int64_t> ids) override { delegate_.Remove(ids); }

private:
  index::FlatIndex delegate_{2, index::MetricType::kL2};
  bool fail_first_add_{true};
};

TEST(LocalShardTest, InsertGetAndSearch) {
  testing::TempDirectory temp_directory;
  auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
  LocalShard shard(temp_directory.path(), std::move(index));

  shard.Put(MakeRecord("v1", 1.0f, 0.0f));
  shard.Put(MakeRecord("v2", 0.0f, 1.0f));
  EXPECT_EQ(shard.size(), 2);

  veclet::v1::VectorRecord fetched;
  ASSERT_TRUE(shard.Get("v1", &fetched));
  EXPECT_EQ(fetched.payload_data(), "metadata");
  EXPECT_FALSE(shard.Get("missing", nullptr));

  const std::vector<float> query = {0.9f, 0.1f};
  const ShardSearchResult result = shard.Search(query, 2);
  ASSERT_EQ(result.hits.size(), 2);
  EXPECT_EQ(result.hits[0].record.vector_id(), "v1");
  EXPECT_EQ(result.hits[1].record.vector_id(), "v2");
  EXPECT_LT(result.hits[0].score, result.hits[1].score);
}

TEST(LocalShardTest, BatchInsertIsAtomicAndExactRetryIsIdempotent) {
  testing::TempDirectory temp_directory;
  auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
  LocalShard shard(temp_directory.path(), std::move(index));
  const std::vector<veclet::v1::VectorRecord> records = {
      MakeRecord("v1", 1.0f, 0.0f), MakeRecord("v2", 0.0f, 1.0f)};

  const ShardPutBatchResult inserted = shard.PutBatch(records);
  EXPECT_EQ(inserted.inserted_records, 2);
  EXPECT_EQ(inserted.duplicate_records, 0);
  EXPECT_EQ(shard.size(), 2);

  const ShardPutBatchResult replay = shard.PutBatch(records);
  EXPECT_EQ(replay.inserted_records, 0);
  EXPECT_EQ(replay.duplicate_records, 2);
  EXPECT_EQ(shard.size(), 2);

  const std::vector<veclet::v1::VectorRecord> mixed = {
      records[0], MakeRecord("v3", -1.0f, 0.0f)};
  const ShardPutBatchResult mixed_result = shard.PutBatch(mixed);
  EXPECT_EQ(mixed_result.inserted_records, 1);
  EXPECT_EQ(mixed_result.duplicate_records, 1);
  EXPECT_EQ(shard.size(), 3);

  const ShardSearchResult result =
      shard.Search(std::vector<float>{-0.9f, 0.0f}, 1);
  ASSERT_EQ(result.hits.size(), 1);
  EXPECT_EQ(result.hits[0].record.vector_id(), "v3");
}

TEST(LocalShardTest, BatchConflictLeavesRocksAndFaissUnchanged) {
  testing::TempDirectory temp_directory;
  auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
  LocalShard shard(temp_directory.path(), std::move(index));
  veclet::v1::VectorRecord existing = MakeRecord("z-existing", 1.0f, 0.0f);
  shard.Put(existing);

  existing.set_version(2);
  const std::vector<veclet::v1::VectorRecord> records = {
      MakeRecord("a-new", 0.0f, 1.0f), existing};
  EXPECT_THROW(shard.PutBatch(records), std::domain_error);
  EXPECT_FALSE(shard.Get("a-new", nullptr));
  EXPECT_EQ(shard.size(), 1);
  const ShardSearchResult result =
      shard.Search(std::vector<float>{0.0f, 1.0f}, 10);
  ASSERT_EQ(result.hits.size(), 1);
  EXPECT_EQ(result.hits[0].record.vector_id(), "z-existing");
}

TEST(LocalShardTest, ExactReplayIsIdempotentAndConflictIsRejected) {
  testing::TempDirectory temp_directory;
  auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
  LocalShard shard(temp_directory.path(), std::move(index));
  veclet::v1::VectorRecord record = MakeRecord("same", 1.0f, 0.0f);

  shard.Put(record);
  shard.Put(record);
  EXPECT_EQ(shard.size(), 1);
  EXPECT_EQ(shard.Search(std::vector<float>{1.0f, 0.0f}, 10).hits.size(), 1);

  record.set_version(2);
  EXPECT_THROW(shard.Put(record), std::domain_error);
  EXPECT_EQ(shard.size(), 1);
}

TEST(LocalShardTest, RecoveryRebuildsDerivedIndexAndMappings) {
  testing::TempDirectory temp_directory;
  {
    auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
    LocalShard shard(temp_directory.path(), std::move(index));
    shard.Put(MakeRecord("doc_1", 10.0f, 0.0f));
    shard.Put(MakeRecord("doc_2", 0.0f, 10.0f));
  }
  {
    auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
    LocalShard shard(temp_directory.path(), std::move(index));
    EXPECT_EQ(shard.size(), 2);
    const ShardSearchResult result =
        shard.Search(std::vector<float>{9.0f, 0.5f}, 1);
    ASSERT_EQ(result.hits.size(), 1);
    EXPECT_EQ(result.hits[0].record.vector_id(), "doc_1");
  }
}

TEST(LocalShardTest, FailedDerivedUpdateFailsClosedUntilRestartRecovery) {
  testing::TempDirectory temp_directory;
  const std::vector<veclet::v1::VectorRecord> records = {
      MakeRecord("committed-1", 1.0f, 0.0f),
      MakeRecord("committed-2", 0.0f, 1.0f)};
  {
    LocalShard shard(temp_directory.path(),
                     std::make_unique<FailFirstAddIndex>());
    EXPECT_THROW(shard.PutBatch(records), std::runtime_error);
    EXPECT_EQ(shard.size(), 2);
    EXPECT_TRUE(shard.Get("committed-1", nullptr));
    EXPECT_TRUE(shard.Get("committed-2", nullptr));
    EXPECT_THROW(shard.Search(std::vector<float>{1.0f, 0.0f}, 1),
                 std::runtime_error);
    EXPECT_THROW(shard.PutBatch(records), std::runtime_error);
    EXPECT_THROW(shard.Put(MakeRecord("not-committed", 0.0f, 1.0f)),
                 std::runtime_error);
    EXPECT_FALSE(shard.Get("not-committed", nullptr));
    EXPECT_EQ(shard.size(), 2);
  }
  {
    auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
    LocalShard recovered(temp_directory.path(), std::move(index));
    const ShardSearchResult result =
        recovered.Search(std::vector<float>{1.0f, 0.0f}, 2);
    ASSERT_EQ(result.hits.size(), 2);
    EXPECT_EQ(result.hits[0].record.vector_id(), "committed-1");
    const ShardPutBatchResult replay = recovered.PutBatch(records);
    EXPECT_EQ(replay.inserted_records, 0);
    EXPECT_EQ(replay.duplicate_records, 2);
    EXPECT_EQ(recovered.size(), 2);
  }
}

TEST(LocalShardTest, RejectsInvalidBatchBeforeWritingAnyRecord) {
  testing::TempDirectory temp_directory;
  auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
  LocalShard shard(temp_directory.path(), std::move(index));

  const std::vector<veclet::v1::VectorRecord> empty;
  EXPECT_THROW(shard.PutBatch(empty), std::invalid_argument);

  const std::vector<veclet::v1::VectorRecord> duplicate_ids = {
      MakeRecord("duplicate", 1.0f, 0.0f), MakeRecord("duplicate", 0.0f, 1.0f)};
  EXPECT_THROW(shard.PutBatch(duplicate_ids), std::invalid_argument);

  veclet::v1::VectorRecord invalid = MakeRecord("invalid", 1.0f, 0.0f);
  invalid.clear_embedding();
  invalid.add_embedding(1.0f);
  const std::vector<veclet::v1::VectorRecord> invalid_later = {
      MakeRecord("valid", 1.0f, 0.0f), invalid};
  EXPECT_THROW(shard.PutBatch(invalid_later), std::invalid_argument);
  EXPECT_FALSE(shard.Get("valid", nullptr));
  EXPECT_EQ(shard.size(), 0);
}

TEST(LocalShardTest, EqualScoresUseExternalIdByteOrdering) {
  testing::TempDirectory temp_directory;
  auto index = std::make_unique<index::FlatIndex>(2, index::MetricType::kL2);
  LocalShard shard(temp_directory.path(), std::move(index));
  shard.Put(MakeRecord("z-last", 0.0f, 1.0f));
  shard.Put(MakeRecord("a-first", 0.0f, -1.0f));
  shard.Put(MakeRecord("m-middle", 1.0f, 0.0f));

  const ShardSearchResult result =
      shard.Search(std::vector<float>{0.0f, 0.0f}, 1);
  ASSERT_EQ(result.hits.size(), 1);
  EXPECT_EQ(result.hits[0].record.vector_id(), "a-first");
}

TEST(LocalShardTest, RejectsUnusableIndexBeforeCreatingRocksDb) {
  testing::TempDirectory temp_directory;
  const std::filesystem::path null_db =
      std::filesystem::path(temp_directory.path()) / "null-index";
  EXPECT_THROW(
      LocalShard(null_db.string(), std::unique_ptr<index::LocalIndex>{}),
      std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(null_db));

  const std::filesystem::path untrained_db =
      std::filesystem::path(temp_directory.path()) / "untrained-index";
  auto untrained =
      std::make_unique<index::IvfFlatIndex>(2, index::MetricType::kL2, 2, 1);
  EXPECT_THROW(LocalShard(untrained_db.string(), std::move(untrained)),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(untrained_db));
}

TEST(LocalShardTest, RejectsInvalidRecordsAndQueries) {
  testing::TempDirectory temp_directory;
  auto index =
      std::make_unique<index::FlatIndex>(2, index::MetricType::kCosine);
  LocalShard shard(temp_directory.path(), std::move(index));

  veclet::v1::VectorRecord record = MakeRecord("valid", 1.0f, 0.0f);
  record.set_vector_id("");
  EXPECT_THROW(shard.Put(record), std::invalid_argument);

  record = MakeRecord(std::string("\xc3\x28", 2), 1.0f, 0.0f);
  EXPECT_THROW(shard.Put(record), std::invalid_argument);

  record = MakeRecord("version-zero", 1.0f, 0.0f);
  record.set_version(0);
  EXPECT_THROW(shard.Put(record), std::invalid_argument);

  record = MakeRecord("wrong-dimension", 1.0f, 0.0f);
  record.clear_embedding();
  record.add_embedding(1.0f);
  EXPECT_THROW(shard.Put(record), std::invalid_argument);

  record =
      MakeRecord("not-finite", 1.0f, std::numeric_limits<float>::quiet_NaN());
  EXPECT_THROW(shard.Put(record), std::invalid_argument);

  record = MakeRecord("zero-cosine", 0.0f, -0.0f);
  EXPECT_THROW(shard.Put(record), std::invalid_argument);

  EXPECT_THROW(shard.Search(std::vector<float>{1.0f}, 1),
               std::invalid_argument);
  EXPECT_THROW(shard.Search(std::vector<float>{1.0f, 0.0f}, 0),
               std::invalid_argument);
  EXPECT_THROW(shard.Search(std::vector<float>{1.0f, 0.0f}, 1001),
               std::invalid_argument);
  EXPECT_EQ(shard.size(), 0);
}

} // namespace
} // namespace veclet::shard
