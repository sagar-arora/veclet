#ifndef VECLET_INDEX_HNSW_FLAT_INDEX_H_
#define VECLET_INDEX_HNSW_FLAT_INDEX_H_

#include "veclet/index/local_index.h"

#include <faiss/IndexIDMap.h>

#include <memory>
#include <unordered_set>

namespace faiss {
struct IndexHNSWFlat;
} // namespace faiss

namespace veclet::index {

class HnswFlatIndex : public LocalIndex {
public:
  HnswFlatIndex(int dimension, MetricType metric, int M = 32,
                int efSearch = 64);
  ~HnswFlatIndex() override;

  // Non-copyable, movable
  HnswFlatIndex(const HnswFlatIndex &) = delete;
  HnswFlatIndex &operator=(const HnswFlatIndex &) = delete;
  HnswFlatIndex(HnswFlatIndex &&) noexcept;
  HnswFlatIndex &operator=(HnswFlatIndex &&) noexcept;

  int dimension() const override { return dimension_; }
  MetricType metric() const override { return metric_; }
  int64_t size() const override;
  bool is_trained() const override { return true; }

  int M() const { return M_; }
  int efSearch() const { return efSearch_; }
  void set_efSearch(int efSearch);

  void Train(std::span<const float> vectors) override;
  void Add(std::span<const int64_t> ids,
           std::span<const float> vectors) override;
  SearchResult Search(std::span<const float> query, int k) const override;
  void Remove(std::span<const int64_t> ids) override;

private:
  int dimension_;
  MetricType metric_;
  int M_;
  int efSearch_;
  faiss::IndexHNSWFlat *raw_hnsw_{nullptr};
  std::unique_ptr<faiss::IndexIDMap2> index_;
  std::unordered_set<int64_t> ids_;
};

} // namespace veclet::index

#endif // VECLET_INDEX_HNSW_FLAT_INDEX_H_
