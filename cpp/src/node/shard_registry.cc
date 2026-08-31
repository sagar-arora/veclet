#include "veclet/node/shard_registry.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace veclet::node {
namespace {

bool IsValidCollectionId(const std::string &collection_id) {
  if (collection_id.empty() || collection_id.size() > 63 ||
      collection_id.front() < 'a' || collection_id.front() > 'z') {
    return false;
  }
  return std::all_of(collection_id.begin() + 1, collection_id.end(),
                     [](unsigned char value) {
                       return (value >= 'a' && value <= 'z') ||
                              (value >= '0' && value <= '9') || value == '-';
                     });
}

bool TargetsEqual(const veclet::v1::ShardTarget &lhs,
                  const veclet::v1::ShardTarget &rhs) {
  return lhs.collection_id() == rhs.collection_id() &&
         lhs.generation_id() == rhs.generation_id() &&
         lhs.shard_id() == rhs.shard_id() &&
         lhs.assignment_epoch() == rhs.assignment_epoch();
}

} // namespace

ShardRegistry::Assignment::Assignment(veclet::v1::ShardTarget target,
                                      std::shared_ptr<shard::LocalShard> shard)
    : target_(std::move(target)), shard_(std::move(shard)) {}

ShardRegistry::~ShardRegistry() {
  std::unique_lock lock(mutex_);
  for (auto &entry : assignments_) {
    entry.second->Revoke();
  }
  assignments_.clear();
}

void ShardRegistry::ValidateTarget(const veclet::v1::ShardTarget &target) {
  if (!IsValidCollectionId(target.collection_id())) {
    throw std::invalid_argument(
        "collection_id must match [a-z][a-z0-9-]{0,62}");
  }
  if (target.generation_id() == 0) {
    throw std::invalid_argument("generation_id must be positive");
  }
  if (target.assignment_epoch() == 0) {
    throw std::invalid_argument("assignment_epoch must be positive");
  }
}

std::string ShardRegistry::MakeKey(const std::string &collection_id,
                                   uint32_t shard_id) {
  return collection_id + ':' + std::to_string(shard_id);
}

void ShardRegistry::Register(const veclet::v1::ShardTarget &target,
                             std::shared_ptr<shard::LocalShard> shard) {
  ValidateTarget(target);
  if (!shard) {
    throw std::invalid_argument("LocalShard pointer must not be null");
  }

  const std::string key = MakeKey(target.collection_id(), target.shard_id());
  std::unique_lock lock(mutex_);
  const auto existing = assignments_.find(key);
  if (existing == assignments_.end()) {
    assignments_.emplace(key, std::shared_ptr<Assignment>(
                                  new Assignment(target, std::move(shard))));
    return;
  }

  const std::shared_ptr<Assignment> &current = existing->second;
  if (target.assignment_epoch() < current->target().assignment_epoch()) {
    throw std::domain_error("stale assignment epoch cannot replace the current "
                            "shard assignment");
  }
  if (target.assignment_epoch() == current->target().assignment_epoch()) {
    if (!TargetsEqual(target, current->target()) || shard != current->shard()) {
      throw std::domain_error(
          "assignment epoch conflicts with the current shard assignment");
    }
    return;
  }
  if (target.generation_id() < current->target().generation_id()) {
    throw std::domain_error(
        "newer assignment epoch cannot move generation backward");
  }

  auto replacement =
      std::shared_ptr<Assignment>(new Assignment(target, std::move(shard)));
  current->Revoke();
  existing->second = std::move(replacement);
}

bool ShardRegistry::Unregister(const veclet::v1::ShardTarget &target) {
  ValidateTarget(target);
  const std::string key = MakeKey(target.collection_id(), target.shard_id());

  std::unique_lock lock(mutex_);
  const auto existing = assignments_.find(key);
  if (existing == assignments_.end() ||
      !TargetsEqual(target, existing->second->target())) {
    return false;
  }

  existing->second->Revoke();
  assignments_.erase(existing);
  return true;
}

ShardRegistry::AssignmentHandle
ShardRegistry::Lookup(const std::string &collection_id,
                      uint32_t shard_id) const {
  if (!IsValidCollectionId(collection_id)) {
    throw std::invalid_argument(
        "collection_id must match [a-z][a-z0-9-]{0,62}");
  }

  const std::string key = MakeKey(collection_id, shard_id);
  std::shared_lock lock(mutex_);
  const auto assignment = assignments_.find(key);
  if (assignment == assignments_.end()) {
    return nullptr;
  }
  return assignment->second;
}

size_t ShardRegistry::size() const {
  std::shared_lock lock(mutex_);
  return assignments_.size();
}

} // namespace veclet::node
