#ifndef VECLET_INDEX_VECTOR_UTILS_H_
#define VECLET_INDEX_VECTOR_UTILS_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <unordered_set>

namespace veclet::index {

inline void ValidateVectors(std::span<const float> vectors, int dimension) {
  if (dimension <= 0) {
    throw std::invalid_argument("Dimension must be positive");
  }
  if (vectors.empty() || vectors.size() % dimension != 0) {
    throw std::invalid_argument(
        "Vector data size must be non-empty and a multiple of dimension");
  }
  for (float v : vectors) {
    if (std::isnan(v) || std::isinf(v)) {
      throw std::invalid_argument("Vector contains NaN or Inf values");
    }
  }
}

inline void NormalizeVector(std::span<float> vec) {
  double norm_sq = 0.0;
  for (float v : vec) {
    norm_sq += static_cast<double>(v) * static_cast<double>(v);
  }
  if (norm_sq == 0.0) {
    throw std::invalid_argument("Cosine vectors must have a non-zero norm");
  }

  const double norm = std::sqrt(norm_sq);
  for (float &v : vec) {
    v = static_cast<float>(static_cast<double>(v) / norm);
  }
}

inline void ValidateNewIds(std::span<const int64_t> ids, size_t expected_size,
                           const std::unordered_set<int64_t> &existing_ids) {
  if (ids.size() != expected_size) {
    throw std::invalid_argument("IDs size does not match number of vectors");
  }

  std::unordered_set<int64_t> batch_ids;
  batch_ids.reserve(ids.size());
  for (int64_t id : ids) {
    if (id <= 0) {
      throw std::invalid_argument("FAISS labels must be positive");
    }
    if (existing_ids.contains(id) || !batch_ids.insert(id).second) {
      throw std::invalid_argument("FAISS labels must be unique");
    }
  }
}

inline void ValidateSearchK(int k) {
  if (k <= 0 || k > 1000) {
    throw std::invalid_argument("k must be between 1 and 1000");
  }
}

} // namespace veclet::index

#endif // VECLET_INDEX_VECTOR_UTILS_H_
