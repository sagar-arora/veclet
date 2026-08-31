#include "veclet/index/flat_index.h"
#include "veclet/index/hnsw_flat_index.h"

#include <gtest/gtest.h>
#include <random>
#include <set>

namespace veclet::index {

TEST(HnswFlatIndexTest, BasicPropertiesAndRemovalGuard) {
  HnswFlatIndex index(2, MetricType::kL2, 16, 32);
  EXPECT_TRUE(index.is_trained());
  EXPECT_EQ(index.size(), 0);
  EXPECT_EQ(index.M(), 16);
  EXPECT_EQ(index.efSearch(), 32);

  std::vector<int64_t> ids = {1, 2};
  std::vector<float> vec = {1.0f, 0.0f, 0.0f, 1.0f};
  index.Add(ids, vec);
  EXPECT_EQ(index.size(), 2);

  // HNSW removal must throw std::domain_error
  EXPECT_THROW(index.Remove(ids), std::domain_error);
}

TEST(HnswFlatIndexTest, RecallAgainstFlatOracle) {
  const int dim = 4;
  const int num_vectors = 200;

  HnswFlatIndex hnsw_index(dim, MetricType::kL2, 16, 32);
  FlatIndex oracle(dim, MetricType::kL2);

  std::mt19937 rng(123);
  std::uniform_real_distribution<float> dist(-10.0f, 10.0f);

  std::vector<int64_t> ids;
  std::vector<float> dataset;
  ids.reserve(num_vectors);
  dataset.reserve(num_vectors * dim);

  for (int i = 0; i < num_vectors; ++i) {
    ids.push_back(i + 1);
    for (int d = 0; d < dim; ++d) {
      dataset.push_back(dist(rng));
    }
  }

  hnsw_index.Add(ids, dataset);
  oracle.Add(ids, dataset);
  EXPECT_EQ(hnsw_index.size(), num_vectors);

  const int k = 5;
  int matched_hits = 0;
  const int num_queries = 20;

  for (int q = 0; q < num_queries; ++q) {
    std::vector<float> query;
    for (int d = 0; d < dim; ++d) {
      query.push_back(dist(rng));
    }

    SearchResult oracle_res = oracle.Search(query, k);
    SearchResult hnsw_res = hnsw_index.Search(query, k);

    std::set<int64_t> oracle_ids;
    for (const auto &hit : oracle_res.hits) {
      oracle_ids.insert(hit.id);
    }

    for (const auto &hit : hnsw_res.hits) {
      if (oracle_ids.count(hit.id) > 0) {
        matched_hits++;
      }
    }
  }

  double recall = static_cast<double>(matched_hits) / (num_queries * k);
  EXPECT_GE(recall, 0.95);
}

TEST(HnswFlatIndexTest, EfSearchValidation) {
  HnswFlatIndex index(2, MetricType::kL2);
  EXPECT_THROW(index.set_efSearch(0), std::invalid_argument);
  EXPECT_THROW(index.set_efSearch(-5), std::invalid_argument);
}

TEST(HnswFlatIndexTest, RejectsDuplicateLabels) {
  HnswFlatIndex index(2, MetricType::kL2);
  index.Add(std::vector<int64_t>{1}, std::vector<float>{0.0f, 0.0f});
  EXPECT_THROW(
      index.Add(std::vector<int64_t>{1}, std::vector<float>{1.0f, 1.0f}),
      std::invalid_argument);
}

} // namespace veclet::index
