#include "veclet/shard/rocks_store.h"

#include "temp_directory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <barrier>
#include <cstddef>
#include <exception>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace veclet::shard {
namespace {

veclet::v1::VectorRecord MakeRecord(std::string vector_id) {
  veclet::v1::VectorRecord record;
  record.set_vector_id(std::move(vector_id));
  record.set_version(1);
  record.add_embedding(1.5f);
  record.add_embedding(-2.5f);
  record.set_payload_data("metadata");
  return record;
}

TEST(RocksStoreTest, AtomicallyStoresBothMappingsAndExactRetry) {
  testing::TempDirectory temp_directory;
  RocksStore store(temp_directory.path());
  const veclet::v1::VectorRecord record = MakeRecord("vec_1");

  const RocksStore::PutResult inserted = store.Put(record);
  EXPECT_TRUE(inserted.inserted);
  EXPECT_EQ(inserted.stored_record.local_index_id(), 1);
  EXPECT_EQ(store.Count(), 1);

  const RocksStore::PutResult replay = store.Put(record);
  EXPECT_FALSE(replay.inserted);
  EXPECT_EQ(replay.stored_record.local_index_id(), 1);
  EXPECT_EQ(store.Count(), 1);

  veclet::storage::v1::StoredRecord by_external_id;
  ASSERT_TRUE(store.Get("vec_1", &by_external_id));
  EXPECT_EQ(by_external_id.record().payload_data(), "metadata");

  veclet::storage::v1::StoredRecord by_local_id;
  ASSERT_TRUE(store.GetByLocalIndexId(1, &by_local_id));
  EXPECT_EQ(by_local_id.record().vector_id(), "vec_1");
  std::string external_id;
  ASSERT_TRUE(store.GetVectorIdByLocalIndexId(1, &external_id));
  EXPECT_EQ(external_id, "vec_1");
  EXPECT_FALSE(store.Get("missing", nullptr));
  EXPECT_FALSE(store.GetVectorIdByLocalIndexId(99, nullptr));
  EXPECT_FALSE(store.GetByLocalIndexId(99, nullptr));
}

TEST(RocksStoreTest, AtomicallyStoresBatchAndPreservesResultOrder) {
  testing::TempDirectory temp_directory;
  RocksStore store(temp_directory.path());
  const std::vector<veclet::v1::VectorRecord> records = {MakeRecord("z-last"),
                                                         MakeRecord("a-first")};

  const RocksStore::PutBatchResult inserted = store.PutBatch(records);
  EXPECT_EQ(inserted.inserted_records, 2);
  EXPECT_EQ(inserted.duplicate_records, 0);
  ASSERT_EQ(inserted.record_results.size(), 2);
  EXPECT_TRUE(inserted.record_results[0].inserted);
  EXPECT_EQ(inserted.record_results[0].stored_record.record().vector_id(),
            "z-last");
  EXPECT_EQ(inserted.record_results[0].stored_record.local_index_id(), 2);
  EXPECT_TRUE(inserted.record_results[1].inserted);
  EXPECT_EQ(inserted.record_results[1].stored_record.record().vector_id(),
            "a-first");
  EXPECT_EQ(inserted.record_results[1].stored_record.local_index_id(), 1);
  EXPECT_EQ(store.Count(), 2);

  const RocksStore::PutBatchResult replay = store.PutBatch(records);
  EXPECT_EQ(replay.inserted_records, 0);
  EXPECT_EQ(replay.duplicate_records, 2);
  ASSERT_EQ(replay.record_results.size(), 2);
  EXPECT_FALSE(replay.record_results[0].inserted);
  EXPECT_EQ(replay.record_results[0].stored_record.local_index_id(), 2);
  EXPECT_FALSE(replay.record_results[1].inserted);
  EXPECT_EQ(replay.record_results[1].stored_record.local_index_id(), 1);

  const std::vector<veclet::v1::VectorRecord> mixed = {records[0],
                                                       MakeRecord("m-middle")};
  const RocksStore::PutBatchResult mixed_result = store.PutBatch(mixed);
  EXPECT_EQ(mixed_result.inserted_records, 1);
  EXPECT_EQ(mixed_result.duplicate_records, 1);
  EXPECT_FALSE(mixed_result.record_results[0].inserted);
  EXPECT_TRUE(mixed_result.record_results[1].inserted);
  EXPECT_EQ(mixed_result.record_results[1].stored_record.local_index_id(), 3);
  EXPECT_EQ(store.Count(), 3);
}

TEST(RocksStoreTest, BatchConflictAbortsEveryWriteAndCounterAdvance) {
  testing::TempDirectory temp_directory;
  RocksStore store(temp_directory.path());
  veclet::v1::VectorRecord existing = MakeRecord("z-existing");
  EXPECT_EQ(store.Put(existing).stored_record.local_index_id(), 1);

  existing.set_version(2);
  const std::vector<veclet::v1::VectorRecord> conflicting = {
      MakeRecord("a-new"), existing};
  EXPECT_THROW(store.PutBatch(conflicting), std::domain_error);
  EXPECT_FALSE(store.Get("a-new", nullptr));
  EXPECT_EQ(store.Count(), 1);

  EXPECT_EQ(
      store.Put(MakeRecord("after-conflict")).stored_record.local_index_id(),
      2);
}

TEST(RocksStoreTest, RejectsInvalidBatchBeforeWritingAnyRecord) {
  testing::TempDirectory temp_directory;
  RocksStore store(temp_directory.path());

  const std::vector<veclet::v1::VectorRecord> empty;
  EXPECT_THROW(store.PutBatch(empty), std::invalid_argument);

  const std::vector<veclet::v1::VectorRecord> duplicate_ids = {
      MakeRecord("duplicate"), MakeRecord("duplicate")};
  EXPECT_THROW(store.PutBatch(duplicate_ids), std::invalid_argument);

  veclet::v1::VectorRecord invalid = MakeRecord("invalid");
  invalid.set_version(0);
  const std::vector<veclet::v1::VectorRecord> invalid_later = {
      MakeRecord("valid"), invalid};
  EXPECT_THROW(store.PutBatch(invalid_later), std::invalid_argument);

  std::vector<veclet::v1::VectorRecord> too_many;
  too_many.reserve(257);
  for (size_t i = 0; i < 257; ++i) {
    too_many.push_back(MakeRecord("id-" + std::to_string(i)));
  }
  EXPECT_THROW(store.PutBatch(too_many), std::invalid_argument);
  EXPECT_EQ(store.Count(), 0);
}

TEST(RocksStoreTest, RejectsConflictingInsertOnlyReplay) {
  testing::TempDirectory temp_directory;
  RocksStore store(temp_directory.path());
  veclet::v1::VectorRecord record = MakeRecord("vec_1");
  store.Put(record);

  record.set_version(2);
  EXPECT_THROW(store.Put(record), std::domain_error);
  EXPECT_EQ(store.Count(), 1);

  veclet::storage::v1::StoredRecord fetched;
  ASSERT_TRUE(store.Get("vec_1", &fetched));
  EXPECT_EQ(fetched.record().version(), 1);
}

TEST(RocksStoreTest, PersistsRecordsMappingsAndCounterAcrossReopen) {
  testing::TempDirectory temp_directory;
  {
    RocksStore store(temp_directory.path());
    EXPECT_EQ(store.Put(MakeRecord("first")).stored_record.local_index_id(), 1);
    EXPECT_EQ(store.Put(MakeRecord("second")).stored_record.local_index_id(),
              2);
  }
  {
    RocksStore store(temp_directory.path());
    EXPECT_EQ(store.Count(), 2);
    EXPECT_EQ(store.Put(MakeRecord("third")).stored_record.local_index_id(), 3);
    veclet::storage::v1::StoredRecord second;
    ASSERT_TRUE(store.GetByLocalIndexId(2, &second));
    EXPECT_EQ(second.record().vector_id(), "second");
  }
}

TEST(RocksStoreTest, ConcurrentInsertsAllocateUniqueLabels) {
  testing::TempDirectory temp_directory;
  RocksStore store(temp_directory.path());
  constexpr size_t kThreadCount = 8;
  std::barrier start(static_cast<std::ptrdiff_t>(kThreadCount));
  std::vector<int64_t> local_ids(kThreadCount);
  std::vector<std::exception_ptr> errors(kThreadCount);
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (size_t i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([&, i] {
      start.arrive_and_wait();
      try {
        local_ids[i] = store.Put(MakeRecord("id-" + std::to_string(i)))
                           .stored_record.local_index_id();
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  for (const std::exception_ptr &error : errors) {
    if (error) {
      std::rethrow_exception(error);
    }
  }

  const std::set<int64_t> unique_ids(local_ids.begin(), local_ids.end());
  EXPECT_EQ(unique_ids.size(), kThreadCount);
  EXPECT_EQ(*unique_ids.begin(), 1);
  EXPECT_EQ(*unique_ids.rbegin(), static_cast<int64_t>(kThreadCount));
  EXPECT_EQ(store.Count(), kThreadCount);
}

TEST(RocksStoreTest, ConcurrentEquivalentBatchesUseStableLockOrder) {
  testing::TempDirectory temp_directory;
  RocksStore store(temp_directory.path());
  std::barrier start(2);
  std::vector<RocksStore::PutBatchResult> results(2);
  std::vector<std::exception_ptr> errors(2);
  const std::vector<veclet::v1::VectorRecord> forward = {MakeRecord("a"),
                                                         MakeRecord("b")};
  const std::vector<veclet::v1::VectorRecord> reverse = {MakeRecord("b"),
                                                         MakeRecord("a")};

  std::thread first([&] {
    start.arrive_and_wait();
    try {
      results[0] = store.PutBatch(forward);
    } catch (...) {
      errors[0] = std::current_exception();
    }
  });
  std::thread second([&] {
    start.arrive_and_wait();
    try {
      results[1] = store.PutBatch(reverse);
    } catch (...) {
      errors[1] = std::current_exception();
    }
  });
  first.join();
  second.join();
  for (const std::exception_ptr &error : errors) {
    if (error) {
      std::rethrow_exception(error);
    }
  }

  EXPECT_EQ(results[0].inserted_records + results[1].inserted_records, 2);
  EXPECT_EQ(results[0].duplicate_records + results[1].duplicate_records, 2);
  EXPECT_EQ(store.Count(), 2);
}

TEST(RocksStoreTest, RejectsInvalidRecordEnvelope) {
  testing::TempDirectory temp_directory;
  RocksStore store(temp_directory.path());

  veclet::v1::VectorRecord record = MakeRecord("valid");
  record.set_version(0);
  EXPECT_THROW(store.Put(record), std::invalid_argument);

  record = MakeRecord(std::string("\xc3\x28", 2));
  EXPECT_THROW(store.Put(record), std::invalid_argument);

  record = MakeRecord("valid");
  record.set_payload_data(std::string(16 * 1024 + 1, 'x'));
  EXPECT_THROW(store.Put(record), std::invalid_argument);
  EXPECT_EQ(store.Count(), 0);
}

} // namespace
} // namespace veclet::shard
