#include "veclet/shard/local_shard.h"

#include "record_validation.h"
#include "veclet/index/vector_utils.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace veclet::shard {

LocalShard::LocalShard(const std::string &db_path,
                       std::unique_ptr<index::LocalIndex> index)
    : index_(std::move(index)), rocks_store_(db_path) {
  if (!index_) {
    throw std::invalid_argument("LocalIndex must not be null");
  }
  if (index_->size() != 0) {
    throw std::invalid_argument(
        "LocalIndex must be empty before LocalShard recovery");
  }
  RecoverDerivedIndex();
}

LocalShard::~LocalShard() = default;

int64_t LocalShard::size() const {
  const size_t count = rocks_store_.Count();
  if (count > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    throw std::overflow_error("RocksDB record count exceeds int64 range");
  }
  return static_cast<int64_t>(count);
}

void LocalShard::RecoverDerivedIndex() {
  std::vector<int64_t> ids;
  std::vector<float> vectors;
  std::unordered_set<int64_t> recovered_ids;

  rocks_store_.Scan(
      [this, &ids, &vectors,
       &recovered_ids](const veclet::storage::v1::StoredRecord &stored_record) {
        ValidateRecordForIndex(stored_record.record(), index_->dimension(),
                               index_->metric());
        const int64_t local_id = stored_record.local_index_id();
        if (!recovered_ids.insert(local_id).second) {
          throw std::runtime_error(
              "Corrupt RocksDB records contain duplicate local_index_id");
        }

        veclet::storage::v1::StoredRecord reverse_record;
        if (!rocks_store_.GetByLocalIndexId(local_id, &reverse_record) ||
            reverse_record.record().vector_id() !=
                stored_record.record().vector_id()) {
          throw std::runtime_error(
              "Corrupt RocksDB record is missing its reverse mapping");
        }
        ids.push_back(local_id);
        vectors.insert(vectors.end(),
                       stored_record.record().embedding().begin(),
                       stored_record.record().embedding().end());
        return true;
      });

  if (ids.empty()) {
    return;
  }
  try {
    if (!index_->is_trained()) {
      index_->Train(vectors);
    }
    index_->Add(ids, vectors);
  } catch (...) {
    std::throw_with_nested(
        std::runtime_error("Failed to rebuild FAISS index from RocksDB"));
  }
  indexed_ids_ = std::move(recovered_ids);
}

void LocalShard::Put(const veclet::v1::VectorRecord &user_record) {
  ValidateRecordForIndex(user_record, index_->dimension(), index_->metric());
  if (!index_->is_trained()) {
    throw std::runtime_error(
        "LocalIndex must be trained before accepting inserts");
  }

  const RocksStore::PutResult put_result = rocks_store_.Put(user_record);

  std::unique_lock lock(index_mutex_);
  if (!derived_index_healthy_) {
    throw std::runtime_error(
        "Derived FAISS index is unavailable; restart is required for recovery");
  }
  const int64_t local_id = put_result.stored_record.local_index_id();
  if (indexed_ids_.contains(local_id)) {
    return;
  }

  const auto &embedding = put_result.stored_record.record().embedding();
  const std::vector<int64_t> ids = {local_id};
  try {
    index_->Add(ids,
                std::span<const float>(embedding.data(), embedding.size()));
    indexed_ids_.insert(local_id);
  } catch (...) {
    derived_index_healthy_ = false;
    std::throw_with_nested(std::runtime_error(
        "RocksDB insert committed but derived FAISS update failed; restart is "
        "required for recovery"));
  }
}

bool LocalShard::Get(const std::string &vector_id,
                     veclet::v1::VectorRecord *record) const {
  veclet::storage::v1::StoredRecord stored_record;
  if (!rocks_store_.Get(vector_id, &stored_record)) {
    return false;
  }
  if (record) {
    *record = stored_record.record();
  }
  return true;
}

ShardSearchResult LocalShard::Search(std::span<const float> query,
                                     int k) const {
  if (query.size() != static_cast<size_t>(index_->dimension())) {
    throw std::invalid_argument("Query size must match shard dimension");
  }
  index::ValidateSearchK(k);

  index::SearchResult index_result;
  {
    std::shared_lock lock(index_mutex_);
    if (!derived_index_healthy_) {
      throw std::runtime_error("Derived FAISS index is unavailable; restart is "
                               "required for recovery");
    }
    index_result = index_->Search(query, k);
  }

  ShardSearchResult shard_result;
  shard_result.hits.reserve(index_result.hits.size());
  for (const index::SearchHit &hit : index_result.hits) {
    veclet::storage::v1::StoredRecord stored_record;
    if (!rocks_store_.GetByLocalIndexId(hit.id, &stored_record)) {
      throw std::runtime_error(
          "FAISS returned a label without an authoritative RocksDB mapping");
    }
    shard_result.hits.push_back(
        {.record = stored_record.record(), .score = hit.score});
  }

  std::sort(shard_result.hits.begin(), shard_result.hits.end(),
            [this](const ShardSearchHit &lhs, const ShardSearchHit &rhs) {
              if (lhs.score != rhs.score) {
                if (index_->metric() == index::MetricType::kL2) {
                  return lhs.score < rhs.score;
                }
                return lhs.score > rhs.score;
              }
              return lhs.record.vector_id() < rhs.record.vector_id();
            });
  return shard_result;
}

} // namespace veclet::shard
