#include "veclet/index/ivf_flat_index.h"
#include "veclet/index/vector_utils.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexIDMap.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/MetricType.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace veclet::index {


IvfFlatIndex::IvfFlatIndex(int dimension, MetricType metric, int nlist, int nprobe)
    : dimension_(dimension), metric_(metric), nlist_(nlist), nprobe_(nprobe) {
  if (dimension <= 0) {
    throw std::invalid_argument("Dimension must be positive");
  }
  if (nlist <= 0) {
    throw std::invalid_argument("nlist must be positive");
  }
  if (nprobe <= 0 || nprobe > nlist) {
    throw std::invalid_argument("nprobe must be positive and less than or equal to nlist");
  }

  faiss::Index* quantizer = nullptr;
  faiss::MetricType faiss_metric = faiss::METRIC_L2;

  if (metric_ == MetricType::kL2) {
    quantizer = new faiss::IndexFlatL2(dimension);
    faiss_metric = faiss::METRIC_L2;
  } else {
    quantizer = new faiss::IndexFlatIP(dimension);
    faiss_metric = faiss::METRIC_INNER_PRODUCT;
  }

  raw_ivf_ = new faiss::IndexIVFFlat(quantizer, dimension, nlist, faiss_metric);
  raw_ivf_->own_fields = true;
  raw_ivf_->nprobe = nprobe;

  index_ = std::make_unique<faiss::IndexIDMap2>(raw_ivf_);
  index_->own_fields = true;
}

IvfFlatIndex::~IvfFlatIndex() = default;

IvfFlatIndex::IvfFlatIndex(IvfFlatIndex&&) noexcept = default;
IvfFlatIndex& IvfFlatIndex::operator=(IvfFlatIndex&&) noexcept = default;

int64_t IvfFlatIndex::size() const {
  return index_ ? index_->ntotal : 0;
}

bool IvfFlatIndex::is_trained() const {
  return raw_ivf_ ? raw_ivf_->is_trained : false;
}

void IvfFlatIndex::set_nprobe(int nprobe) {
  if (nprobe <= 0 || nprobe > nlist_) {
    throw std::invalid_argument("nprobe must be positive and less than or equal to nlist");
  }
  nprobe_ = nprobe;
  if (raw_ivf_) {
    raw_ivf_->nprobe = nprobe_;
  }
}

void IvfFlatIndex::Train(std::span<const float> vectors) {
  ValidateVectors(vectors, dimension_);
  const size_t num_vectors = vectors.size() / dimension_;
  if (num_vectors < static_cast<size_t>(nlist_)) {
    throw std::invalid_argument("Training set size must be at least equal to nlist");
  }

  std::vector<float> processed_vectors(vectors.begin(), vectors.end());
  if (metric_ == MetricType::kCosine) {
    for (size_t i = 0; i < num_vectors; ++i) {
      std::span<float> vec(&processed_vectors[i * dimension_], dimension_);
      NormalizeVector(vec);
    }
  }

  index_->train(static_cast<faiss::idx_t>(num_vectors), processed_vectors.data());
}

void IvfFlatIndex::Add(std::span<const int64_t> ids, std::span<const float> vectors) {
  if (!is_trained()) {
    throw std::runtime_error("Index must be trained before calling Add");
  }

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

SearchResult IvfFlatIndex::Search(std::span<const float> query, int k) const {
  if (!is_trained()) {
    throw std::runtime_error("Index must be trained before calling Search");
  }

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

  raw_ivf_->nprobe = nprobe_;
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

void IvfFlatIndex::Remove(std::span<const int64_t> ids) {
  if (ids.empty()) {
    return;
  }
  faiss::IDSelectorBatch selector(
      ids.size(),
      reinterpret_cast<const faiss::idx_t*>(ids.data()));
  index_->remove_ids(selector);
}

}  // namespace veclet::index
