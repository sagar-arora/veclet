#include "veclet/node/shard_registry.h"

#include "temp_directory.h"
#include "veclet/index/flat_index.h"

#include <gtest/gtest.h>

#include <barrier>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace veclet::node {
namespace {

veclet::v1::ShardPlacement MakePlacement(uint64_t generation_id,
                                         uint64_t placement_epoch) {
  veclet::v1::ShardPlacement placement;
  placement.set_collection_id("products");
  placement.set_generation_id(generation_id);
  placement.set_shard_id(2);
  placement.set_placement_epoch(placement_epoch);
  return placement;
}

std::shared_ptr<shard::LocalShard>
MakeShard(const testing::TempDirectory &temp_directory,
          const std::string &name) {
  const std::filesystem::path db_path =
      std::filesystem::path(temp_directory.path()) / name;
  return std::make_shared<shard::LocalShard>(
      db_path.string(),
      std::make_unique<index::FlatIndex>(2, index::MetricType::kL2));
}

TEST(ShardRegistryTest, InstallsLooksUpAndRepeatsExactPlacement) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const veclet::v1::ShardPlacement placement = MakePlacement(7, 20);
  const auto shard = MakeShard(temp_directory, "current");

  registry.Install(placement, shard);
  registry.Install(placement, shard);

  ASSERT_EQ(registry.size(), 1);
  const ShardRegistry::PlacementHandle installed =
      registry.Lookup("products", 2);
  ASSERT_NE(installed, nullptr);
  EXPECT_TRUE(installed->active());
  EXPECT_EQ(installed->placement().generation_id(), 7);
  EXPECT_EQ(installed->placement().placement_epoch(), 20);
  EXPECT_EQ(installed->shard(), shard);
  EXPECT_EQ(registry.Lookup("products", 3), nullptr);
}

TEST(ShardRegistryTest, NewerEpochReplacesAndRevokesPreviousHandle) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const auto first_shard = MakeShard(temp_directory, "first");
  const auto second_shard = MakeShard(temp_directory, "second");

  registry.Install(MakePlacement(7, 20), first_shard);
  const ShardRegistry::PlacementHandle previous =
      registry.Lookup("products", 2);
  ASSERT_NE(previous, nullptr);

  registry.Install(MakePlacement(7, 21), second_shard);

  EXPECT_FALSE(previous->active());
  const ShardRegistry::PlacementHandle current = registry.Lookup("products", 2);
  ASSERT_NE(current, nullptr);
  EXPECT_TRUE(current->active());
  EXPECT_EQ(current->placement().placement_epoch(), 21);
  EXPECT_EQ(current->shard(), second_shard);
}

TEST(ShardRegistryTest, RejectsStaleAndConflictingReplacement) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const veclet::v1::ShardPlacement current = MakePlacement(7, 20);
  const auto current_shard = MakeShard(temp_directory, "current");
  registry.Install(current, current_shard);

  EXPECT_THROW(registry.Install(MakePlacement(7, 19), current_shard),
               std::domain_error);
  EXPECT_THROW(registry.Install(MakePlacement(8, 20), current_shard),
               std::domain_error);
  EXPECT_THROW(registry.Install(current, MakeShard(temp_directory,
                                                   "same-epoch-other-shard")),
               std::domain_error);
  EXPECT_THROW(registry.Install(MakePlacement(6, 21), current_shard),
               std::domain_error);

  const ShardRegistry::PlacementHandle installed =
      registry.Lookup("products", 2);
  ASSERT_NE(installed, nullptr);
  EXPECT_TRUE(installed->active());
  EXPECT_EQ(installed->placement().generation_id(), 7);
  EXPECT_EQ(installed->placement().placement_epoch(), 20);
}

TEST(ShardRegistryTest, StaleRemoveCannotRemoveNewerPlacement) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const veclet::v1::ShardPlacement old_placement = MakePlacement(7, 20);
  const veclet::v1::ShardPlacement new_placement = MakePlacement(7, 21);
  registry.Install(old_placement, MakeShard(temp_directory, "old"));
  registry.Install(new_placement, MakeShard(temp_directory, "new"));

  EXPECT_FALSE(registry.Remove(old_placement));
  const ShardRegistry::PlacementHandle current = registry.Lookup("products", 2);
  ASSERT_NE(current, nullptr);
  EXPECT_TRUE(current->active());
  EXPECT_EQ(current->placement().placement_epoch(), 21);

  EXPECT_TRUE(registry.Remove(new_placement));
  EXPECT_FALSE(current->active());
  EXPECT_EQ(registry.Lookup("products", 2), nullptr);
  EXPECT_EQ(registry.size(), 0);
  EXPECT_FALSE(registry.Remove(new_placement));
}

TEST(ShardRegistryTest, RejectsInvalidPlacementsAndNullShard) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const auto shard = MakeShard(temp_directory, "valid");

  veclet::v1::ShardPlacement placement = MakePlacement(7, 20);
  placement.set_collection_id("Products");
  EXPECT_THROW(registry.Install(placement, shard), std::invalid_argument);

  placement = MakePlacement(7, 20);
  placement.set_collection_id("products:other");
  EXPECT_THROW(registry.Install(placement, shard), std::invalid_argument);

  placement = MakePlacement(0, 20);
  EXPECT_THROW(registry.Install(placement, shard), std::invalid_argument);

  placement = MakePlacement(7, 0);
  EXPECT_THROW(registry.Install(placement, shard), std::invalid_argument);

  EXPECT_THROW(registry.Install(MakePlacement(7, 20), nullptr),
               std::invalid_argument);
  EXPECT_THROW(registry.Lookup("Products", 2), std::invalid_argument);
  EXPECT_EQ(registry.size(), 0);
}

TEST(ShardRegistryTest, DestructionRevokesOutstandingHandles) {
  testing::TempDirectory temp_directory;
  ShardRegistry::PlacementHandle placement;
  {
    ShardRegistry registry;
    registry.Install(MakePlacement(7, 20),
                     MakeShard(temp_directory, "current"));
    placement = registry.Lookup("products", 2);
    ASSERT_NE(placement, nullptr);
    EXPECT_TRUE(placement->active());
  }
  EXPECT_FALSE(placement->active());
}

TEST(ShardRegistryTest, ConcurrentLookupHandleIsRevokedSafely) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  registry.Install(MakePlacement(7, 20), MakeShard(temp_directory, "current"));

  std::barrier lookup_complete(2);
  std::barrier replacement_complete(2);
  ShardRegistry::PlacementHandle observed;
  bool active_after_replacement = true;
  std::thread reader([&] {
    observed = registry.Lookup("products", 2);
    lookup_complete.arrive_and_wait();
    replacement_complete.arrive_and_wait();
    active_after_replacement = observed->active();
  });

  lookup_complete.arrive_and_wait();
  registry.Install(MakePlacement(7, 21),
                   MakeShard(temp_directory, "replacement"));
  replacement_complete.arrive_and_wait();
  reader.join();

  ASSERT_NE(observed, nullptr);
  EXPECT_FALSE(active_after_replacement);
  const ShardRegistry::PlacementHandle current = registry.Lookup("products", 2);
  ASSERT_NE(current, nullptr);
  EXPECT_TRUE(current->active());
  EXPECT_EQ(current->placement().placement_epoch(), 21);
}

} // namespace
} // namespace veclet::node
