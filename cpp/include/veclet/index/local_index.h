#ifndef VECLET_INDEX_LOCAL_INDEX_H_
#define VECLET_INDEX_LOCAL_INDEX_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace veclet::index {

enum class MetricType {
  kL2,
  kInnerProduct,
  kCosine,
};

struct SearchHit {
  int64_t id{0};
  float score{0.0f};

  bool operator==(const SearchHit &other) const {
    return id == other.id && score == other.score;
  }
};

struct SearchResult {
  std::vector<SearchHit> hits;
};

class LocalIndex {
public:
  virtual ~LocalIndex() = default;

  // LocalIndex implementations are not internally synchronized. Concurrent
  // Search calls are supported after configuration is complete. The owner
  // must exclude Train, Add, Remove, and configuration setters from every
  // other operation.

  virtual int dimension() const = 0;
  virtual MetricType metric() const = 0;
  virtual int64_t size() const = 0;
  virtual bool is_trained() const = 0;

  virtual void Train(std::span<const float> vectors) = 0;
  virtual void Add(std::span<const int64_t> ids,
                   std::span<const float> vectors) = 0;
  // k may exceed the public QueryService limit when the shard needs extra
  // candidates to resolve an equal-score boundary by external vector ID.
  virtual SearchResult Search(std::span<const float> query, int k) const = 0;
  virtual void Remove(std::span<const int64_t> ids) = 0;
};

} // namespace veclet::index

#endif // VECLET_INDEX_LOCAL_INDEX_H_
