#ifndef VECLET_NODE_REPLICA_MANAGER_H_
#define VECLET_NODE_REPLICA_MANAGER_H_

#include "veclet/artifact/replica_artifact.h"
#include "veclet/node/data_node.h"

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>

namespace veclet::node {

enum class ReplicaState {
  kRecovering,
  kReady,
  kFailed,
};

enum class PreparePlacementOutcome {
  kReady,
  kAlreadyReady,
  kAlreadyRecovering,
  kCancelled,
  kSuperseded,
};

struct ReplicaStatus {
  ReplicaState state{ReplicaState::kRecovering};
  veclet::v1::ShardPlacement desired_placement;
  std::string manifest_id;
  std::optional<veclet::v1::ShardPlacement> active_placement;
  std::string failure;
};

// ReplicaManager serializes desired placement state per logical shard while
// allowing artifact loads for different shards to run concurrently. Its mutex
// is never held during ReplicaLoader or RocksDB/FAISS work. The only nested
// lock order is ReplicaManager -> ShardRegistry during the short final install
// or exact remove operation; RPC paths never acquire ReplicaManager's mutex.
//
// The owner must stop and join every thread calling this object before
// destruction. ReplicaManager owns no worker thread; a future control-stream
// executor owns calls and their deadline.
class ReplicaManager {
public:
  ReplicaManager(DataNodeService &service,
                 std::unique_ptr<artifact::ReplicaLoader> loader);
  ~ReplicaManager();

  ReplicaManager(const ReplicaManager &) = delete;
  ReplicaManager &operator=(const ReplicaManager &) = delete;
  ReplicaManager(ReplicaManager &&) = delete;
  ReplicaManager &operator=(ReplicaManager &&) = delete;

  PreparePlacementOutcome
  PreparePlacement(const artifact::ReplicaLoadRequest &request,
                   std::stop_token stop_token = {});

  // Cancels recovery or removes the active registry entry only when placement
  // exactly matches. A delayed remove cannot affect a newer placement.
  bool RemovePlacement(const veclet::v1::ShardPlacement &placement);

  std::optional<ReplicaStatus> Status(const std::string &collection_id,
                                      uint32_t shard_id) const;

private:
  struct Slot;

  static std::string MakeKey(const std::string &collection_id,
                             uint32_t shard_id);

  DataNodeService &service_;
  std::unique_ptr<artifact::ReplicaLoader> loader_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::unique_ptr<Slot>> slots_;
};

} // namespace veclet::node

#endif // VECLET_NODE_REPLICA_MANAGER_H_
