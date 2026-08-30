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
  EXPECT_FALSE(store.Get("missing", nullptr));
  EXPECT_FALSE(store.GetByLocalIndexId(99, nullptr));
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
