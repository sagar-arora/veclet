#ifndef VECLET_INDEX_VECTOR_UTILS_H_
#define VECLET_INDEX_VECTOR_UTILS_H_

#include <span>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace veclet::index {

inline void ValidateVectors(std::span<const float> vectors, int dimension) {
  if (dimension <= 0) {
    throw std::invalid_argument("Dimension must be positive");
  }
  if (vectors.empty() || vectors.size() % dimension != 0) {
    throw std::invalid_argument("Vector data size must be non-empty and a multiple of dimension");
  }
  for (float v : vectors) {
    if (std::isnan(v) || std::isinf(v)) {
      throw std::invalid_argument("Vector contains NaN or Inf values");
    }
  }
}

inline void NormalizeVector(std::span<float> vec) {
  float norm_sq = 0.0f;
  for (float v : vec) {
    norm_sq += v * v;
  }
  if (norm_sq > 0.0f) {
    float norm = std::sqrt(norm_sq);
    for (float& v : vec) {
      v /= norm;
    }
  }
}

}  // namespace veclet::index

#endif  // VECLET_INDEX_VECTOR_UTILS_H_
