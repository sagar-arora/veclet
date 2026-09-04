#ifndef VECLET_NODE_SHARD_REGISTRY_H_
#define VECLET_NODE_SHARD_REGISTRY_H_

#include "veclet/shard/local_shard.h"
#include "veclet/v1/data.pb.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace veclet::node {

class ShardRegistry {
public:
  class Placement {
  public:
    const veclet::v1::ShardPlacement &placement() const { return placement_; }
    const std::shared_ptr<shard::LocalShard> &shard() const { return shard_; }
    bool active() const noexcept {
      return active_.load(std::memory_order_acquire);
    }

  private:
    friend class ShardRegistry;

    Placement(veclet::v1::ShardPlacement placement,
              std::shared_ptr<shard::LocalShard> shard);
    void Revoke() noexcept { active_.store(false, std::memory_order_release); }

    veclet::v1::ShardPlacement placement_;
    std::shared_ptr<shard::LocalShard> shard_;
    std::atomic<bool> active_{true};
  };

  using PlacementHandle = std::shared_ptr<const Placement>;

  ShardRegistry() = default;
  ~ShardRegistry();

  ShardRegistry(const ShardRegistry &) = delete;
  ShardRegistry &operator=(const ShardRegistry &) = delete;
  ShardRegistry(ShardRegistry &&) = delete;
  ShardRegistry &operator=(ShardRegistry &&) = delete;

  // Install publishes an already-READY LocalShard on this DataNode. It performs
  // no recovery or remote I/O. Repeating the exact placement with the same
  // LocalShard is idempotent. A replacement requires a strictly newer
  // placement epoch and may not move generation backward.
  void Install(const veclet::v1::ShardPlacement &placement,
               std::shared_ptr<shard::LocalShard> shard);

  // Remove revokes and removes only the exact current placement. A stale
  // command returns false and cannot remove a newer placement.
  bool Remove(const veclet::v1::ShardPlacement &placement);

  // Lookup returns a lifetime-safe handle without retaining the registry lock.
  // RPC owners must check active() before work and again before acknowledging.
  PlacementHandle Lookup(const std::string &collection_id,
                         uint32_t shard_id) const;

  size_t size() const;

private:
  static void ValidatePlacement(const veclet::v1::ShardPlacement &placement);
  static std::string MakeKey(const std::string &collection_id,
                             uint32_t shard_id);

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Placement>> placements_;
};

} // namespace veclet::node

#endif // VECLET_NODE_SHARD_REGISTRY_H_
