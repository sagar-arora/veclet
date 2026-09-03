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

bool PlacementsEqual(const veclet::v1::ShardPlacement &lhs,
                     const veclet::v1::ShardPlacement &rhs) {
  return lhs.collection_id() == rhs.collection_id() &&
         lhs.generation_id() == rhs.generation_id() &&
         lhs.shard_id() == rhs.shard_id() &&
         lhs.placement_epoch() == rhs.placement_epoch();
}

} // namespace

ShardRegistry::Placement::Placement(veclet::v1::ShardPlacement placement,
                                    std::shared_ptr<shard::LocalShard> shard)
    : placement_(std::move(placement)), shard_(std::move(shard)) {}

ShardRegistry::~ShardRegistry() {
  std::unique_lock lock(mutex_);
  for (auto &entry : placements_) {
    entry.second->Revoke();
  }
  placements_.clear();
}

void ShardRegistry::ValidatePlacement(
    const veclet::v1::ShardPlacement &placement) {
  if (!IsValidCollectionId(placement.collection_id())) {
    throw std::invalid_argument(
        "collection_id must match [a-z][a-z0-9-]{0,62}");
  }
  if (placement.generation_id() == 0) {
    throw std::invalid_argument("generation_id must be positive");
  }
  if (placement.placement_epoch() == 0) {
    throw std::invalid_argument("placement_epoch must be positive");
  }
}

std::string ShardRegistry::MakeKey(const std::string &collection_id,
                                   uint32_t shard_id) {
  return collection_id + ':' + std::to_string(shard_id);
}

void ShardRegistry::Install(const veclet::v1::ShardPlacement &placement,
                            std::shared_ptr<shard::LocalShard> shard) {
  ValidatePlacement(placement);
  if (!shard) {
    throw std::invalid_argument("LocalShard pointer must not be null");
  }

  const std::string key =
      MakeKey(placement.collection_id(), placement.shard_id());
  std::unique_lock lock(mutex_);
  const auto existing = placements_.find(key);
  if (existing == placements_.end()) {
    placements_.emplace(key, std::shared_ptr<Placement>(
                                 new Placement(placement, std::move(shard))));
    return;
  }

  const std::shared_ptr<Placement> &current = existing->second;
  if (placement.placement_epoch() < current->placement().placement_epoch()) {
    throw std::domain_error(
        "stale placement epoch cannot replace the current shard placement");
  }
  if (placement.placement_epoch() == current->placement().placement_epoch()) {
    if (!PlacementsEqual(placement, current->placement()) ||
        shard != current->shard()) {
      throw std::domain_error(
          "placement epoch conflicts with the current shard placement");
    }
    return;
  }
  if (placement.generation_id() < current->placement().generation_id()) {
    throw std::domain_error(
        "newer placement epoch cannot move generation backward");
  }

  auto replacement =
      std::shared_ptr<Placement>(new Placement(placement, std::move(shard)));
  current->Revoke();
  existing->second = std::move(replacement);
}

bool ShardRegistry::Remove(const veclet::v1::ShardPlacement &placement) {
  ValidatePlacement(placement);
  const std::string key =
      MakeKey(placement.collection_id(), placement.shard_id());

  std::unique_lock lock(mutex_);
  const auto existing = placements_.find(key);
  if (existing == placements_.end() ||
      !PlacementsEqual(placement, existing->second->placement())) {
    return false;
  }

  existing->second->Revoke();
  placements_.erase(existing);
  return true;
}

ShardRegistry::PlacementHandle
ShardRegistry::Lookup(const std::string &collection_id,
                      uint32_t shard_id) const {
  if (!IsValidCollectionId(collection_id)) {
    throw std::invalid_argument(
        "collection_id must match [a-z][a-z0-9-]{0,62}");
  }

  const std::string key = MakeKey(collection_id, shard_id);
  std::shared_lock lock(mutex_);
  const auto placement = placements_.find(key);
  if (placement == placements_.end()) {
    return nullptr;
  }
  return placement->second;
}

size_t ShardRegistry::size() const {
  std::shared_lock lock(mutex_);
  return placements_.size();
}

} // namespace veclet::node
