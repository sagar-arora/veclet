#ifndef VECLET_INDEX_IVF_FLAT_INDEX_H_
#define VECLET_INDEX_IVF_FLAT_INDEX_H_

#include "veclet/index/local_index.h"

#include <memory>
#include <vector>

namespace faiss {
struct IndexIDMap2;
struct IndexIVFFlat;
}  // namespace faiss

namespace veclet::index {

class IvfFlatIndex : public LocalIndex {
 public:
  IvfFlatIndex(int dimension, MetricType metric, int nlist = 100, int nprobe = 10);
  ~IvfFlatIndex() override;

  // Non-copyable, movable
  IvfFlatIndex(const IvfFlatIndex&) = delete;
  IvfFlatIndex& operator=(const IvfFlatIndex&) = delete;
  IvfFlatIndex(IvfFlatIndex&&) noexcept;
  IvfFlatIndex& operator=(IvfFlatIndex&&) noexcept;

  int dimension() const override { return dimension_; }
  MetricType metric() const override { return metric_; }
  int64_t size() const override;
  bool is_trained() const override;

  int nlist() const { return nlist_; }
  int nprobe() const { return nprobe_; }
  void set_nprobe(int nprobe);

  void Train(std::span<const float> vectors) override;
  void Add(std::span<const int64_t> ids, std::span<const float> vectors) override;
  SearchResult Search(std::span<const float> query, int k) const override;
  void Remove(std::span<const int64_t> ids) override;

 private:
  int dimension_;
  MetricType metric_;
  int nlist_;
  int nprobe_;
  faiss::IndexIVFFlat* raw_ivf_{nullptr};
  std::unique_ptr<faiss::IndexIDMap2> index_;
};

}  // namespace veclet::index

#endif  // VECLET_INDEX_IVF_FLAT_INDEX_H_
