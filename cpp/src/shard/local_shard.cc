#include "veclet/shard/local_shard.h"

#include "record_validation.h"
#include "veclet/index/vector_utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace veclet::shard {
namespace {

constexpr int kMaxPublicSearchK = 1000;
constexpr size_t kRecoveryBatchVectorLimit = 1024;
constexpr size_t kRecoveryBatchFloatLimit = 1U << 20;

std::unique_ptr<index::LocalIndex>
RequireRecoveryIndex(std::unique_ptr<index::LocalIndex> index) {
  if (!index) {
    throw std::invalid_argument("LocalIndex must not be null");
  }
  if (index->dimension() <= 0) {
    throw std::invalid_argument("LocalIndex dimension must be positive");
  }
  if (index->size() != 0) {
    throw std::invalid_argument(
        "LocalIndex must be empty before LocalShard recovery");
  }
  if (!index->is_trained()) {
    throw std::invalid_argument(
        "LocalIndex must be trained before LocalShard recovery");
  }
  return index;
}

bool ScoresEqualAtBoundary(const index::SearchResult &result, int k) {
  return result.hits.size() > static_cast<size_t>(k) &&
         result.hits[k - 1].score == result.hits.back().score;
}

struct ResolvedSearchHit {
  int64_t local_id;
  std::string vector_id;
  float score;
};

} // namespace

LocalShard::LocalShard(const std::string &db_path,
                       std::unique_ptr<index::LocalIndex> index)
    : index_(RequireRecoveryIndex(std::move(index))), rocks_store_(db_path) {
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
  const size_t dimension = static_cast<size_t>(index_->dimension());
  const size_t batch_size =
      std::max<size_t>(1, std::min(kRecoveryBatchVectorLimit,
                                   kRecoveryBatchFloatLimit / dimension));

  ids.reserve(batch_size);
  vectors.reserve(batch_size * dimension);

  const auto flush_batch = [this, &ids, &vectors]() {
    if (ids.empty()) {
      return;
    }
    index_->Add(ids, vectors);
    ids.clear();
    vectors.clear();
  };

  try {
    rocks_store_.Scan(
        [this, &ids, &vectors, &flush_batch,
         batch_size](const veclet::storage::v1::StoredRecord &stored_record) {
          ValidateRecordForIndex(stored_record.record(), index_->dimension(),
                                 index_->metric());
          const int64_t local_id = stored_record.local_index_id();
          if (!indexed_ids_.insert(local_id).second) {
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
          if (ids.size() == batch_size) {
            flush_batch();
          }
          return true;
        });
    flush_batch();
  } catch (...) {
    std::throw_with_nested(
        std::runtime_error("Failed to rebuild FAISS index from RocksDB"));
  }
}

void LocalShard::Put(const veclet::v1::VectorRecord &user_record) {
  (void)PutBatch(std::span<const veclet::v1::VectorRecord>(&user_record, 1));
}

ShardPutBatchResult
LocalShard::PutBatch(std::span<const veclet::v1::VectorRecord> records) {
  constexpr size_t kMaxBatchRecords = 256;
  if (records.empty() || records.size() > kMaxBatchRecords) {
    throw std::invalid_argument("batch must contain 1 to 256 records");
  }
  const size_t dimension = static_cast<size_t>(index_->dimension());
  if (records.size() > std::numeric_limits<size_t>::max() / dimension) {
    throw std::overflow_error("batch embedding size exceeds size_t range");
  }
  for (const veclet::v1::VectorRecord &record : records) {
    ValidateRecordForIndex(record, index_->dimension(), index_->metric());
  }
  if (!index_->is_trained()) {
    throw std::runtime_error(
        "LocalIndex must be trained before accepting inserts");
  }
  {
    std::shared_lock lock(index_mutex_);
    if (!derived_index_healthy_) {
      throw std::runtime_error("Derived FAISS index is unavailable; restart is "
                               "required for recovery");
    }
  }

  std::vector<int64_t> local_ids;
  std::vector<int64_t> missing_ids;
  std::vector<float> missing_vectors;
  local_ids.reserve(records.size());
  missing_ids.reserve(records.size());
  missing_vectors.reserve(records.size() * dimension);

  const RocksStore::PutBatchResult put_result = rocks_store_.PutBatch(records);
  for (const RocksStore::PutResult &record_result : put_result.record_results) {
    local_ids.push_back(record_result.stored_record.local_index_id());
  }

  std::unique_lock lock(index_mutex_);
  if (!derived_index_healthy_) {
    throw std::runtime_error(
        "Derived FAISS index is unavailable; restart is required for recovery");
  }
  for (size_t i = 0; i < local_ids.size(); ++i) {
    if (!indexed_ids_.contains(local_ids[i])) {
      missing_ids.push_back(local_ids[i]);
      const auto &embedding = records[i].embedding();
      missing_vectors.insert(missing_vectors.end(), embedding.begin(),
                             embedding.end());
    }
  }
  if (missing_ids.empty()) {
    return {.inserted_records = put_result.inserted_records,
            .duplicate_records = put_result.duplicate_records};
  }

  try {
    if (missing_ids.size() > indexed_ids_.max_size() - indexed_ids_.size()) {
      throw std::overflow_error("indexed local-ID set exceeds size range");
    }
    indexed_ids_.reserve(indexed_ids_.size() + missing_ids.size());
    index_->Add(missing_ids, missing_vectors);
    indexed_ids_.insert(missing_ids.begin(), missing_ids.end());
  } catch (...) {
    derived_index_healthy_ = false;
    std::throw_with_nested(std::runtime_error(
        "RocksDB batch committed but derived FAISS update failed; restart is "
        "required for recovery"));
  }
  return {.inserted_records = put_result.inserted_records,
          .duplicate_records = put_result.duplicate_records};
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
  if (k <= 0 || k > kMaxPublicSearchK) {
    throw std::invalid_argument("k must be between 1 and 1000");
  }

  index::SearchResult index_result;
  {
    std::shared_lock lock(index_mutex_);
    if (!derived_index_healthy_) {
      throw std::runtime_error("Derived FAISS index is unavailable; restart is "
                               "required for recovery");
    }
    const int64_t index_size = index_->size();
    if (index_size < 0) {
      throw std::runtime_error("LocalIndex returned a negative size");
    }

    int candidate_k = static_cast<int>(std::min<int64_t>(
        index_size, std::max<int64_t>(k, static_cast<int64_t>(k) + 1)));
    if (candidate_k > 0) {
      while (true) {
        index_result = index_->Search(query, candidate_k);
        if (index_result.hits.size() < static_cast<size_t>(candidate_k) ||
            candidate_k == index_size ||
            !ScoresEqualAtBoundary(index_result, k)) {
          break;
        }
        if (candidate_k == index::kMaxIndexSearchCandidates) {
          throw std::runtime_error(
              "Equal-score search boundary exceeds the 10000-candidate "
              "safety limit");
        }
        candidate_k = static_cast<int>(std::min<int64_t>(
            index_size,
            std::min<int64_t>(index::kMaxIndexSearchCandidates,
                              static_cast<int64_t>(candidate_k) * 2)));
      }
    }
  }

  std::vector<ResolvedSearchHit> resolved_hits;
  resolved_hits.reserve(index_result.hits.size());
  for (const index::SearchHit &hit : index_result.hits) {
    std::string vector_id;
    if (!rocks_store_.GetVectorIdByLocalIndexId(hit.id, &vector_id)) {
      throw std::runtime_error(
          "FAISS returned a label without an authoritative RocksDB mapping");
    }
    resolved_hits.push_back({.local_id = hit.id,
                             .vector_id = std::move(vector_id),
                             .score = hit.score});
  }

  std::sort(resolved_hits.begin(), resolved_hits.end(),
            [this](const ResolvedSearchHit &lhs, const ResolvedSearchHit &rhs) {
              if (lhs.score != rhs.score) {
                if (index_->metric() == index::MetricType::kL2) {
                  return lhs.score < rhs.score;
                }
                return lhs.score > rhs.score;
              }
              return lhs.vector_id < rhs.vector_id;
            });
  if (resolved_hits.size() > static_cast<size_t>(k)) {
    resolved_hits.resize(static_cast<size_t>(k));
  }

  ShardSearchResult shard_result;
  shard_result.hits.reserve(resolved_hits.size());
  for (const ResolvedSearchHit &resolved_hit : resolved_hits) {
    veclet::storage::v1::StoredRecord stored_record;
    if (!rocks_store_.GetByLocalIndexId(resolved_hit.local_id,
                                        &stored_record) ||
        stored_record.record().vector_id() != resolved_hit.vector_id) {
      throw std::runtime_error(
          "RocksDB mapping changed while resolving a search result");
    }
    shard_result.hits.push_back(
        {.record = std::move(*stored_record.mutable_record()),
         .score = resolved_hit.score});
  }
  return shard_result;
}

} // namespace veclet::shard
