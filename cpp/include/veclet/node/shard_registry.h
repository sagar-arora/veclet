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
  class Assignment {
  public:
    const veclet::v1::ShardTarget &target() const { return target_; }
    const std::shared_ptr<shard::LocalShard> &shard() const { return shard_; }
    bool active() const noexcept {
      return active_.load(std::memory_order_acquire);
    }

  private:
    friend class ShardRegistry;

    Assignment(veclet::v1::ShardTarget target,
               std::shared_ptr<shard::LocalShard> shard);
    void Revoke() noexcept { active_.store(false, std::memory_order_release); }

    veclet::v1::ShardTarget target_;
    std::shared_ptr<shard::LocalShard> shard_;
    std::atomic<bool> active_{true};
  };

  using AssignmentHandle = std::shared_ptr<const Assignment>;

  ShardRegistry() = default;
  ~ShardRegistry();

  ShardRegistry(const ShardRegistry &) = delete;
  ShardRegistry &operator=(const ShardRegistry &) = delete;
  ShardRegistry(ShardRegistry &&) = delete;
  ShardRegistry &operator=(ShardRegistry &&) = delete;

  // Register installs a READY local assignment. Repeating the exact target
  // with the same LocalShard is idempotent. A replacement requires a strictly
  // newer assignment epoch and may not move generation backward.
  void Register(const veclet::v1::ShardTarget &target,
                std::shared_ptr<shard::LocalShard> shard);

  // Unregister revokes and removes only the exact registered target. A stale
  // command returns false and cannot remove a newer assignment.
  bool Unregister(const veclet::v1::ShardTarget &target);

  // Lookup returns a lifetime-safe handle without retaining the registry lock.
  // RPC owners must check active() before work and again before acknowledging.
  AssignmentHandle Lookup(const std::string &collection_id,
                          uint32_t shard_id) const;

  size_t size() const;

private:
  static void ValidateTarget(const veclet::v1::ShardTarget &target);
  static std::string MakeKey(const std::string &collection_id,
                             uint32_t shard_id);

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Assignment>> assignments_;
};

} // namespace veclet::node

#endif // VECLET_NODE_SHARD_REGISTRY_H_
