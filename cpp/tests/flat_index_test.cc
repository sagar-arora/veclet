#include "veclet/index/flat_index.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>

namespace veclet::index {

TEST(FlatIndexTest, L2ExactRecallAndSorting) {
  FlatIndex index(2, MetricType::kL2);
  EXPECT_EQ(index.dimension(), 2);
  EXPECT_EQ(index.metric(), MetricType::kL2);
  EXPECT_EQ(index.size(), 0);
  EXPECT_TRUE(index.is_trained());

  std::vector<int64_t> ids = {1, 2, 3};
  std::vector<float> vectors = {
      1.0f, 0.0f, // ID 1
      0.0f, 1.0f, // ID 2
      1.0f, 1.0f  // ID 3
  };

  index.Add(ids, vectors);
  EXPECT_EQ(index.size(), 3);

  // Query close to ID 1
  std::vector<float> query = {0.9f, 0.1f};
  SearchResult result = index.Search(query, 2);

  ASSERT_EQ(result.hits.size(), 2);
  EXPECT_EQ(result.hits[0].id, 1);
  EXPECT_EQ(result.hits[1].id, 3);
  EXPECT_LT(result.hits[0].score, result.hits[1].score); // L2: lower is better
}

TEST(FlatIndexTest, InnerProductExactRecall) {
  FlatIndex index(2, MetricType::kInnerProduct);

  std::vector<int64_t> ids = {10, 20};
  std::vector<float> vectors = {
      1.0f, 0.0f, // ID 10
      0.5f, 0.5f  // ID 20
  };

  index.Add(ids, vectors);

  std::vector<float> query = {1.0f, 0.0f};
  SearchResult result = index.Search(query, 2);

  ASSERT_EQ(result.hits.size(), 2);
  EXPECT_EQ(result.hits[0].id, 10);
  EXPECT_EQ(result.hits[1].id, 20);
  EXPECT_GT(result.hits[0].score, result.hits[1].score); // IP: higher is better
}

TEST(FlatIndexTest, CosineNormalizationAndSearch) {
  FlatIndex index(2, MetricType::kCosine);

  std::vector<int64_t> ids = {100, 200};
  std::vector<float> vectors = {
      10.0f, 0.0f, // ID 100 (unnormalized)
      0.0f, 5.0f   // ID 200 (unnormalized)
  };

  index.Add(ids, vectors);

  // Query pointing parallel to ID 100
  std::vector<float> query = {2.0f, 0.1f};
  SearchResult result = index.Search(query, 1);

  ASSERT_EQ(result.hits.size(), 1);
  EXPECT_EQ(result.hits[0].id, 100);
}

TEST(FlatIndexTest, DeterministicTieBreaking) {
  FlatIndex index(2, MetricType::kL2);

  // Two vectors equidistant from query [0.0, 0.0]
  std::vector<int64_t> ids = {5, 2};
  std::vector<float> vectors = {
      0.0f, 1.0f, // ID 5 (dist^2 = 1.0)
      0.0f, -1.0f // ID 2 (dist^2 = 1.0)
  };

  index.Add(ids, vectors);

  std::vector<float> query = {0.0f, 0.0f};
  SearchResult result = index.Search(query, 2);

  ASSERT_EQ(result.hits.size(), 2);
  EXPECT_FLOAT_EQ(result.hits[0].score, result.hits[1].score);
  // Lower ID should break the tie
  EXPECT_EQ(result.hits[0].id, 2);
  EXPECT_EQ(result.hits[1].id, 5);
}

TEST(FlatIndexTest, InvalidInputHandling) {
  EXPECT_THROW(FlatIndex(0, MetricType::kL2), std::invalid_argument);
  EXPECT_THROW(FlatIndex(2, static_cast<MetricType>(99)),
               std::invalid_argument);

  FlatIndex index(2, MetricType::kL2);

  // NaN in input
  std::vector<int64_t> ids = {1};
  std::vector<float> nan_vec = {1.0f, std::numeric_limits<float>::quiet_NaN()};
  EXPECT_THROW(index.Add(ids, nan_vec), std::invalid_argument);

  // Inf in input
  std::vector<float> inf_vec = {std::numeric_limits<float>::infinity(), 1.0f};
  EXPECT_THROW(index.Add(ids, inf_vec), std::invalid_argument);

  // Dimension mismatch in Add
  std::vector<float> wrong_dim = {1.0f, 2.0f, 3.0f};
  EXPECT_THROW(index.Add(ids, wrong_dim), std::invalid_argument);

  // Valid Add for search tests
  std::vector<float> valid_vec = {1.0f, 2.0f};
  index.Add(ids, valid_vec);

  // Dimension mismatch in Search
  std::vector<float> wrong_query = {1.0f};
  EXPECT_THROW(index.Search(wrong_query, 1), std::invalid_argument);

  // Invalid k
  std::vector<float> valid_query = {1.0f, 2.0f};
  EXPECT_THROW(index.Search(valid_query, 0), std::invalid_argument);
  EXPECT_THROW(index.Search(valid_query, -1), std::invalid_argument);
  EXPECT_THROW(index.Search(valid_query, 1001), std::invalid_argument);
}

TEST(FlatIndexTest, RejectsNonPositiveAndDuplicateLabels) {
  FlatIndex index(2, MetricType::kL2);
  EXPECT_THROW(
      index.Add(std::vector<int64_t>{0}, std::vector<float>{1.0f, 0.0f}),
      std::invalid_argument);
  EXPECT_THROW(
      index.Add(std::vector<int64_t>{-1}, std::vector<float>{1.0f, 0.0f}),
      std::invalid_argument);
  EXPECT_THROW(index.Add(std::vector<int64_t>{1, 1},
                         std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f}),
               std::invalid_argument);

  index.Add(std::vector<int64_t>{1}, std::vector<float>{1.0f, 0.0f});
  EXPECT_THROW(
      index.Add(std::vector<int64_t>{1}, std::vector<float>{0.0f, 1.0f}),
      std::invalid_argument);
  EXPECT_EQ(index.size(), 1);
}

TEST(FlatIndexTest, RejectsZeroNormCosineVectorsAndQueries) {
  FlatIndex index(2, MetricType::kCosine);
  EXPECT_THROW(
      index.Add(std::vector<int64_t>{1}, std::vector<float>{0.0f, -0.0f}),
      std::invalid_argument);

  index.Add(std::vector<int64_t>{1}, std::vector<float>{1.0f, 0.0f});
  EXPECT_THROW(index.Search(std::vector<float>{0.0f, 0.0f}, 1),
               std::invalid_argument);
}

TEST(FlatIndexTest, CloseScoresUseMetricOrderingRatherThanEpsilonTie) {
  FlatIndex index(1, MetricType::kL2);
  index.Add(std::vector<int64_t>{1, 2}, std::vector<float>{1.0000002f, 1.0f});

  const SearchResult result = index.Search(std::vector<float>{0.0f}, 2);
  ASSERT_EQ(result.hits.size(), 2);
  EXPECT_EQ(result.hits[0].id, 2);
  EXPECT_LT(result.hits[0].score, result.hits[1].score);
}

TEST(FlatIndexTest, RemoveVectors) {
  FlatIndex index(2, MetricType::kL2);

  std::vector<int64_t> ids = {1, 2};
  std::vector<float> vectors = {1.0f, 0.0f, 0.0f, 1.0f};
  index.Add(ids, vectors);
  EXPECT_EQ(index.size(), 2);

  std::vector<int64_t> to_remove = {1};
  index.Remove(to_remove);
  EXPECT_EQ(index.size(), 1);

  std::vector<float> query = {1.0f, 0.0f};
  SearchResult result = index.Search(query, 2);

  ASSERT_EQ(result.hits.size(), 1);
  EXPECT_EQ(result.hits[0].id, 2);
}

} // namespace veclet::index
