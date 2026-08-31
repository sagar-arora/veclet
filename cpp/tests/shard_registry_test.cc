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

veclet::v1::ShardTarget MakeTarget(uint64_t generation_id,
                                   uint64_t assignment_epoch) {
  veclet::v1::ShardTarget target;
  target.set_collection_id("products");
  target.set_generation_id(generation_id);
  target.set_shard_id(2);
  target.set_assignment_epoch(assignment_epoch);
  return target;
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

TEST(ShardRegistryTest, RegistersLooksUpAndRepeatsExactAssignment) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const veclet::v1::ShardTarget target = MakeTarget(7, 20);
  const auto shard = MakeShard(temp_directory, "current");

  registry.Register(target, shard);
  registry.Register(target, shard);

  ASSERT_EQ(registry.size(), 1);
  const ShardRegistry::AssignmentHandle assignment =
      registry.Lookup("products", 2);
  ASSERT_NE(assignment, nullptr);
  EXPECT_TRUE(assignment->active());
  EXPECT_EQ(assignment->target().generation_id(), 7);
  EXPECT_EQ(assignment->target().assignment_epoch(), 20);
  EXPECT_EQ(assignment->shard(), shard);
  EXPECT_EQ(registry.Lookup("products", 3), nullptr);
}

TEST(ShardRegistryTest, NewerEpochReplacesAndRevokesPreviousHandle) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const auto first_shard = MakeShard(temp_directory, "first");
  const auto second_shard = MakeShard(temp_directory, "second");

  registry.Register(MakeTarget(7, 20), first_shard);
  const ShardRegistry::AssignmentHandle previous =
      registry.Lookup("products", 2);
  ASSERT_NE(previous, nullptr);

  registry.Register(MakeTarget(7, 21), second_shard);

  EXPECT_FALSE(previous->active());
  const ShardRegistry::AssignmentHandle current =
      registry.Lookup("products", 2);
  ASSERT_NE(current, nullptr);
  EXPECT_TRUE(current->active());
  EXPECT_EQ(current->target().assignment_epoch(), 21);
  EXPECT_EQ(current->shard(), second_shard);
}

TEST(ShardRegistryTest, RejectsStaleAndConflictingReplacement) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const veclet::v1::ShardTarget current = MakeTarget(7, 20);
  const auto current_shard = MakeShard(temp_directory, "current");
  registry.Register(current, current_shard);

  EXPECT_THROW(registry.Register(MakeTarget(7, 19), current_shard),
               std::domain_error);
  EXPECT_THROW(registry.Register(MakeTarget(8, 20), current_shard),
               std::domain_error);
  EXPECT_THROW(registry.Register(current, MakeShard(temp_directory,
                                                    "same-epoch-other-shard")),
               std::domain_error);
  EXPECT_THROW(registry.Register(MakeTarget(6, 21), current_shard),
               std::domain_error);

  const ShardRegistry::AssignmentHandle assignment =
      registry.Lookup("products", 2);
  ASSERT_NE(assignment, nullptr);
  EXPECT_TRUE(assignment->active());
  EXPECT_EQ(assignment->target().generation_id(), 7);
  EXPECT_EQ(assignment->target().assignment_epoch(), 20);
}

TEST(ShardRegistryTest, StaleUnregisterCannotRemoveNewerAssignment) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const veclet::v1::ShardTarget old_target = MakeTarget(7, 20);
  const veclet::v1::ShardTarget new_target = MakeTarget(7, 21);
  registry.Register(old_target, MakeShard(temp_directory, "old"));
  registry.Register(new_target, MakeShard(temp_directory, "new"));

  EXPECT_FALSE(registry.Unregister(old_target));
  const ShardRegistry::AssignmentHandle current =
      registry.Lookup("products", 2);
  ASSERT_NE(current, nullptr);
  EXPECT_TRUE(current->active());
  EXPECT_EQ(current->target().assignment_epoch(), 21);

  EXPECT_TRUE(registry.Unregister(new_target));
  EXPECT_FALSE(current->active());
  EXPECT_EQ(registry.Lookup("products", 2), nullptr);
  EXPECT_EQ(registry.size(), 0);
  EXPECT_FALSE(registry.Unregister(new_target));
}

TEST(ShardRegistryTest, RejectsInvalidTargetsAndNullShard) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  const auto shard = MakeShard(temp_directory, "valid");

  veclet::v1::ShardTarget target = MakeTarget(7, 20);
  target.set_collection_id("Products");
  EXPECT_THROW(registry.Register(target, shard), std::invalid_argument);

  target = MakeTarget(7, 20);
  target.set_collection_id("products:other");
  EXPECT_THROW(registry.Register(target, shard), std::invalid_argument);

  target = MakeTarget(0, 20);
  EXPECT_THROW(registry.Register(target, shard), std::invalid_argument);

  target = MakeTarget(7, 0);
  EXPECT_THROW(registry.Register(target, shard), std::invalid_argument);

  EXPECT_THROW(registry.Register(MakeTarget(7, 20), nullptr),
               std::invalid_argument);
  EXPECT_THROW(registry.Lookup("Products", 2), std::invalid_argument);
  EXPECT_EQ(registry.size(), 0);
}

TEST(ShardRegistryTest, DestructionRevokesOutstandingHandles) {
  testing::TempDirectory temp_directory;
  ShardRegistry::AssignmentHandle assignment;
  {
    ShardRegistry registry;
    registry.Register(MakeTarget(7, 20), MakeShard(temp_directory, "current"));
    assignment = registry.Lookup("products", 2);
    ASSERT_NE(assignment, nullptr);
    EXPECT_TRUE(assignment->active());
  }
  EXPECT_FALSE(assignment->active());
}

TEST(ShardRegistryTest, ConcurrentLookupHandleIsRevokedSafely) {
  testing::TempDirectory temp_directory;
  ShardRegistry registry;
  registry.Register(MakeTarget(7, 20), MakeShard(temp_directory, "current"));

  std::barrier lookup_complete(2);
  std::barrier replacement_complete(2);
  ShardRegistry::AssignmentHandle observed;
  bool active_after_replacement = true;
  std::thread reader([&] {
    observed = registry.Lookup("products", 2);
    lookup_complete.arrive_and_wait();
    replacement_complete.arrive_and_wait();
    active_after_replacement = observed->active();
  });

  lookup_complete.arrive_and_wait();
  registry.Register(MakeTarget(7, 21),
                    MakeShard(temp_directory, "replacement"));
  replacement_complete.arrive_and_wait();
  reader.join();

  ASSERT_NE(observed, nullptr);
  EXPECT_FALSE(active_after_replacement);
  const ShardRegistry::AssignmentHandle current =
      registry.Lookup("products", 2);
  ASSERT_NE(current, nullptr);
  EXPECT_TRUE(current->active());
  EXPECT_EQ(current->target().assignment_epoch(), 21);
}

} // namespace
} // namespace veclet::node
