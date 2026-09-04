#include "veclet/index/flat_index.h"
#include "veclet/index/vector_utils.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexIDMap.h>
#include <faiss/MetricType.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace veclet::index {

FlatIndex::FlatIndex(int dimension, MetricType metric)
    : dimension_(dimension), metric_(metric) {
  if (dimension <= 0) {
    throw std::invalid_argument("Dimension must be positive");
  }

  faiss::Index *sub_index;
  switch (metric_) {
  case MetricType::kL2:
    sub_index = new faiss::IndexFlatL2(dimension);
    break;
  case MetricType::kInnerProduct:
  case MetricType::kCosine:
    sub_index = new faiss::IndexFlatIP(dimension);
    break;
  default:
    throw std::invalid_argument("Unsupported metric type");
  }

  index_ = std::make_unique<faiss::IndexIDMap2>(sub_index);
  index_->own_fields = true;
}

FlatIndex::~FlatIndex() = default;

FlatIndex::FlatIndex(FlatIndex &&) noexcept = default;
FlatIndex &FlatIndex::operator=(FlatIndex &&) noexcept = default;

int64_t FlatIndex::size() const { return index_ ? index_->ntotal : 0; }

void FlatIndex::Train([[maybe_unused]] std::span<const float> vectors) {
  // Flat indexes are always trained and require no training phase.
}

void FlatIndex::Add(std::span<const int64_t> ids,
                    std::span<const float> vectors) {
  ValidateVectors(vectors, dimension_);
  const size_t num_vectors = vectors.size() / dimension_;
  ValidateNewIds(ids, num_vectors, ids_);

  std::vector<float> processed_vectors(vectors.begin(), vectors.end());
  if (metric_ == MetricType::kCosine) {
    for (size_t i = 0; i < num_vectors; ++i) {
      std::span<float> vec(&processed_vectors[i * dimension_], dimension_);
      NormalizeVector(vec);
    }
  }

  index_->add_with_ids(static_cast<faiss::idx_t>(num_vectors),
                       processed_vectors.data(),
                       reinterpret_cast<const faiss::idx_t *>(ids.data()));
  ids_.insert(ids.begin(), ids.end());
}

SearchResult FlatIndex::Search(std::span<const float> query, int k) const {
  ValidateVectors(query, dimension_);
  if (query.size() != static_cast<size_t>(dimension_)) {
    throw std::invalid_argument("Query size must match index dimension");
  }
  ValidateSearchK(k);

  std::vector<float> processed_query(query.begin(), query.end());
  if (metric_ == MetricType::kCosine) {
    NormalizeVector(processed_query);
  }

  std::vector<float> distances(k);
  std::vector<faiss::idx_t> labels(k);

  index_->search(1, processed_query.data(), k, distances.data(), labels.data());

  SearchResult result;
  for (int i = 0; i < k; ++i) {
    if (labels[i] >= 0) {
      if (!std::isfinite(distances[i])) {
        throw std::runtime_error("FAISS returned a non-finite score");
      }
      SearchHit hit;
      hit.id = labels[i];
      hit.score = distances[i];
      result.hits.push_back(hit);
    }
  }

  // Deterministic tie-breaking and sorting
  std::sort(result.hits.begin(), result.hits.end(),
            [this](const SearchHit &a, const SearchHit &b) {
              if (a.score != b.score) {
                if (metric_ == MetricType::kL2) {
                  return a.score < b.score; // L2: lower is better
                } else {
                  return a.score >
                         b.score; // InnerProduct/Cosine: higher is better
                }
              }
              return a.id < b.id; // Tie-breaker: lower ID is preferred
            });

  return result;
}

void FlatIndex::Remove(std::span<const int64_t> ids) {
  if (ids.empty()) {
    return;
  }
  faiss::IDSelectorBatch selector(
      ids.size(), reinterpret_cast<const faiss::idx_t *>(ids.data()));
  index_->remove_ids(selector);
  for (int64_t id : ids) {
    ids_.erase(id);
  }
}

} // namespace veclet::index
