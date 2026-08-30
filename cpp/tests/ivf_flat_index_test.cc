#include "veclet/index/flat_index.h"
#include "veclet/index/ivf_flat_index.h"

#include <gtest/gtest.h>
#include <random>
#include <set>

namespace veclet::index {

TEST(IvfFlatIndexTest, PreTrainingGuards) {
  IvfFlatIndex index(2, MetricType::kL2, 10, 5);
  EXPECT_FALSE(index.is_trained());
  EXPECT_EQ(index.size(), 0);

  std::vector<int64_t> ids = {1};
  std::vector<float> vec = {1.0f, 2.0f};

  // Add and Search before training must throw
  EXPECT_THROW(index.Add(ids, vec), std::runtime_error);
  EXPECT_THROW(index.Search(vec, 1), std::runtime_error);
}

TEST(IvfFlatIndexTest, TrainingAndRecallAgainstFlatOracle) {
  const int dim = 4;
  const int num_vectors = 200;
  const int nlist = 10;
  const int nprobe = 10;

  IvfFlatIndex ivf_index(dim, MetricType::kL2, nlist, nprobe);
  FlatIndex oracle(dim, MetricType::kL2);

  // Generate synthetic training vectors
  std::mt19937 rng(42);
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

  // Train and Add
  ivf_index.Train(dataset);
  EXPECT_TRUE(ivf_index.is_trained());

  ivf_index.Add(ids, dataset);
  oracle.Add(ids, dataset);
  EXPECT_EQ(ivf_index.size(), num_vectors);

  // Perform search queries and measure recall against oracle
  const int k = 5;
  int matched_hits = 0;
  const int num_queries = 20;

  for (int q = 0; q < num_queries; ++q) {
    std::vector<float> query;
    for (int d = 0; d < dim; ++d) {
      query.push_back(dist(rng));
    }

    SearchResult oracle_res = oracle.Search(query, k);
    SearchResult ivf_res = ivf_index.Search(query, k);

    std::set<int64_t> oracle_ids;
    for (const auto &hit : oracle_res.hits) {
      oracle_ids.insert(hit.id);
    }

    for (const auto &hit : ivf_res.hits) {
      if (oracle_ids.count(hit.id) > 0) {
        matched_hits++;
      }
    }
  }

  double recall = static_cast<double>(matched_hits) / (num_queries * k);
  EXPECT_GE(recall, 0.95);
}

TEST(IvfFlatIndexTest, InvalidNprobe) {
  IvfFlatIndex index(2, MetricType::kL2, 10, 5);
  EXPECT_THROW(index.set_nprobe(0), std::invalid_argument);
  EXPECT_THROW(index.set_nprobe(11), std::invalid_argument);
}

TEST(IvfFlatIndexTest, RejectsDuplicateLabelsAfterTraining) {
  IvfFlatIndex index(2, MetricType::kL2, 2, 1);
  const std::vector<float> training = {0.0f, 0.0f, 1.0f, 1.0f};
  index.Train(training);
  index.Add(std::vector<int64_t>{1}, std::vector<float>{0.0f, 0.0f});
  EXPECT_THROW(
      index.Add(std::vector<int64_t>{1}, std::vector<float>{1.0f, 1.0f}),
      std::invalid_argument);
}

} // namespace veclet::index
