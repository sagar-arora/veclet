#ifndef VECLET_SHARD_RECORD_VALIDATION_H_
#define VECLET_SHARD_RECORD_VALIDATION_H_

#include "veclet/index/local_index.h"
#include "veclet/v1/common.pb.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace veclet::shard {

inline bool IsValidUtf8(std::string_view value) {
  const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
  size_t i = 0;
  while (i < value.size()) {
    const unsigned char first = bytes[i];
    if (first <= 0x7f) {
      ++i;
      continue;
    }

    size_t continuation_count = 0;
    unsigned char second_min = 0x80;
    unsigned char second_max = 0xbf;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation_count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation_count = 2;
      if (first == 0xe0) {
        second_min = 0xa0;
      } else if (first == 0xed) {
        second_max = 0x9f;
      }
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation_count = 3;
      if (first == 0xf0) {
        second_min = 0x90;
      } else if (first == 0xf4) {
        second_max = 0x8f;
      }
    } else {
      return false;
    }

    if (i + continuation_count >= value.size() || bytes[i + 1] < second_min ||
        bytes[i + 1] > second_max) {
      return false;
    }
    for (size_t offset = 2; offset <= continuation_count; ++offset) {
      if (bytes[i + offset] < 0x80 || bytes[i + offset] > 0xbf) {
        return false;
      }
    }
    i += continuation_count + 1;
  }
  return true;
}

inline void ValidateRecordEnvelope(const veclet::v1::VectorRecord &record) {
  if (record.vector_id().empty() || record.vector_id().size() > 256 ||
      !IsValidUtf8(record.vector_id())) {
    throw std::invalid_argument(
        "vector_id must contain 1 to 256 valid UTF-8 bytes");
  }
  if (record.version() == 0) {
    throw std::invalid_argument("version must be positive");
  }
  if (record.embedding().empty()) {
    throw std::invalid_argument("embedding must not be empty");
  }
  if (!std::all_of(record.embedding().begin(), record.embedding().end(),
                   [](float value) { return std::isfinite(value); })) {
    throw std::invalid_argument("embedding contains NaN or Inf values");
  }
  if (record.payload_data().size() > 16 * 1024 ||
      !IsValidUtf8(record.payload_data())) {
    throw std::invalid_argument(
        "payload_data must contain at most 16 KiB of valid UTF-8");
  }
}

inline void ValidateRecordForIndex(const veclet::v1::VectorRecord &record,
                                   int dimension, index::MetricType metric) {
  ValidateRecordEnvelope(record);
  if (record.embedding_size() != dimension) {
    throw std::invalid_argument(
        "embedding dimension does not match shard index dimension");
  }
  if (metric == index::MetricType::kCosine &&
      std::all_of(record.embedding().begin(), record.embedding().end(),
                  [](float value) { return value == 0.0f; })) {
    throw std::invalid_argument("Cosine vectors must have a non-zero norm");
  }
}

inline bool RecordsEqual(const veclet::v1::VectorRecord &lhs,
                         const veclet::v1::VectorRecord &rhs) {
  return lhs.vector_id() == rhs.vector_id() && lhs.version() == rhs.version() &&
         lhs.payload_data() == rhs.payload_data() &&
         lhs.embedding_size() == rhs.embedding_size() &&
         std::equal(lhs.embedding().begin(), lhs.embedding().end(),
                    rhs.embedding().begin());
}

} // namespace veclet::shard

#endif // VECLET_SHARD_RECORD_VALIDATION_H_
