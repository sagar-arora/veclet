#ifndef VECLET_SHARD_LOCAL_SHARD_H_
#define VECLET_SHARD_LOCAL_SHARD_H_

#include "veclet/index/local_index.h"
#include "veclet/shard/rocks_store.h"
#include "veclet/v1/common.pb.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace veclet::shard {

struct ShardSearchHit {
  veclet::v1::VectorRecord record;
  float score{0.0f};

  bool operator==(const ShardSearchHit &other) const {
    return record.vector_id() == other.record.vector_id() &&
           score == other.score;
  }
};

struct ShardSearchResult {
  std::vector<ShardSearchHit> hits;
};

struct ShardPutBatchResult {
  size_t inserted_records{0};
  size_t duplicate_records{0};
};

class LocalShard {
public:
  LocalShard(const std::string &db_path,
             std::unique_ptr<index::LocalIndex> index);
  ~LocalShard();

  // Non-copyable, non-movable
  LocalShard(const LocalShard &) = delete;
  LocalShard &operator=(const LocalShard &) = delete;
  LocalShard(LocalShard &&) = delete;
  LocalShard &operator=(LocalShard &&) = delete;

  // LocalShard supports concurrent Put, PutBatch, Get, and Search calls.
  // RocksDB work is never performed while index_mutex_ is held. FAISS
  // mutations take the exclusive lock; searches take the shared lock.
  int dimension() const { return index_->dimension(); }
  index::MetricType metric() const { return index_->metric(); }
  int64_t size() const;

  void Put(const veclet::v1::VectorRecord &user_record);
  ShardPutBatchResult
  PutBatch(std::span<const veclet::v1::VectorRecord> records);
  bool Get(const std::string &vector_id,
           veclet::v1::VectorRecord *record) const;
  ShardSearchResult Search(std::span<const float> query, int k) const;

private:
  std::unique_ptr<index::LocalIndex> index_;
  RocksStore rocks_store_;
  mutable std::shared_mutex index_mutex_;
  std::unordered_set<int64_t> indexed_ids_;
  bool derived_index_healthy_{true};

  void RecoverDerivedIndex();
};

} // namespace veclet::shard

#endif // VECLET_SHARD_LOCAL_SHARD_H_
