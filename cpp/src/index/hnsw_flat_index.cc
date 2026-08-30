#include "veclet/index/hnsw_flat_index.h"
#include "veclet/index/vector_utils.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>
#include <faiss/IndexIDMap.h>
#include <faiss/MetricType.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace veclet::index {


HnswFlatIndex::HnswFlatIndex(int dimension, MetricType metric, int M, int efSearch)
    : dimension_(dimension), metric_(metric), M_(M), efSearch_(efSearch) {
  if (dimension <= 0) {
    throw std::invalid_argument("Dimension must be positive");
  }
  if (M <= 0) {
    throw std::invalid_argument("M must be positive");
  }
  if (efSearch <= 0) {
    throw std::invalid_argument("efSearch must be positive");
  }

  faiss::MetricType faiss_metric = faiss::METRIC_L2;
  if (metric_ != MetricType::kL2) {
    faiss_metric = faiss::METRIC_INNER_PRODUCT;
  }

  raw_hnsw_ = new faiss::IndexHNSWFlat(dimension, M, faiss_metric);
  raw_hnsw_->hnsw.efSearch = efSearch;

  index_ = std::make_unique<faiss::IndexIDMap2>(raw_hnsw_);
  index_->own_fields = true;
}

HnswFlatIndex::~HnswFlatIndex() = default;

HnswFlatIndex::HnswFlatIndex(HnswFlatIndex&&) noexcept = default;
HnswFlatIndex& HnswFlatIndex::operator=(HnswFlatIndex&&) noexcept = default;

int64_t HnswFlatIndex::size() const {
  return index_ ? index_->ntotal : 0;
}

void HnswFlatIndex::set_efSearch(int efSearch) {
  if (efSearch <= 0) {
    throw std::invalid_argument("efSearch must be positive");
  }
  efSearch_ = efSearch;
  if (raw_hnsw_) {
    raw_hnsw_->hnsw.efSearch = efSearch_;
  }
}

void HnswFlatIndex::Train([[maybe_unused]] std::span<const float> vectors) {
  // HNSW indexes do not require a separate training step.
}

void HnswFlatIndex::Add(std::span<const int64_t> ids, std::span<const float> vectors) {
  ValidateVectors(vectors, dimension_);
  const size_t num_vectors = vectors.size() / dimension_;
  if (ids.size() != num_vectors) {
    throw std::invalid_argument("IDs size does not match number of vectors");
  }

  std::vector<float> processed_vectors(vectors.begin(), vectors.end());
  if (metric_ == MetricType::kCosine) {
    for (size_t i = 0; i < num_vectors; ++i) {
      std::span<float> vec(&processed_vectors[i * dimension_], dimension_);
      NormalizeVector(vec);
    }
  }

  index_->add_with_ids(
      static_cast<faiss::idx_t>(num_vectors),
      processed_vectors.data(),
      reinterpret_cast<const faiss::idx_t*>(ids.data()));
}

SearchResult HnswFlatIndex::Search(std::span<const float> query, int k) const {
  ValidateVectors(query, dimension_);
  if (query.size() != static_cast<size_t>(dimension_)) {
    throw std::invalid_argument("Query size must match index dimension");
  }
  if (k <= 0) {
    throw std::invalid_argument("k must be positive");
  }

  std::vector<float> processed_query(query.begin(), query.end());
  if (metric_ == MetricType::kCosine) {
    NormalizeVector(processed_query);
  }

  std::vector<float> distances(k);
  std::vector<faiss::idx_t> labels(k);

  raw_hnsw_->hnsw.efSearch = efSearch_;
  index_->search(
      1,
      processed_query.data(),
      k,
      distances.data(),
      labels.data());

  SearchResult result;
  for (int i = 0; i < k; ++i) {
    if (labels[i] >= 0) {
      SearchHit hit;
      hit.id = labels[i];
      hit.score = distances[i];
      hit.vector_id = std::to_string(labels[i]);
      result.hits.push_back(hit);
    }
  }

  // Deterministic tie-breaking and sorting
  std::sort(result.hits.begin(), result.hits.end(), [this](const SearchHit& a, const SearchHit& b) {
    if (std::abs(a.score - b.score) > 1e-6f) {
      if (metric_ == MetricType::kL2) {
        return a.score < b.score;  // L2: lower is better
      } else {
        return a.score > b.score;  // InnerProduct/Cosine: higher is better
      }
    }
    return a.id < b.id;  // Tie-breaker: lower ID is preferred
  });

  return result;
}

void HnswFlatIndex::Remove([[maybe_unused]] std::span<const int64_t> ids) {
  throw std::domain_error("HnswFlatIndex does not support vector removal");
}

}  // namespace veclet::index
