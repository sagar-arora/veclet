#ifndef VECLET_INDEX_FLAT_INDEX_H_
#define VECLET_INDEX_FLAT_INDEX_H_

#include "veclet/index/local_index.h"

#include <memory>
#include <vector>

namespace faiss {
struct IndexIDMap2;
}  // namespace faiss

namespace veclet::index {

class FlatIndex : public LocalIndex {
 public:
  FlatIndex(int dimension, MetricType metric);
  ~FlatIndex() override;

  // Non-copyable, movable
  FlatIndex(const FlatIndex&) = delete;
  FlatIndex& operator=(const FlatIndex&) = delete;
  FlatIndex(FlatIndex&&) noexcept;
  FlatIndex& operator=(FlatIndex&&) noexcept;

  int dimension() const override { return dimension_; }
  MetricType metric() const override { return metric_; }
  int64_t size() const override;
  bool is_trained() const override { return true; }

  void Train(std::span<const float> vectors) override;
  void Add(std::span<const int64_t> ids, std::span<const float> vectors) override;
  SearchResult Search(std::span<const float> query, int k) const override;
  void Remove(std::span<const int64_t> ids) override;

 private:
  int dimension_;
  MetricType metric_;
  std::unique_ptr<faiss::IndexIDMap2> index_;
};

}  // namespace veclet::index

#endif  // VECLET_INDEX_FLAT_INDEX_H_
