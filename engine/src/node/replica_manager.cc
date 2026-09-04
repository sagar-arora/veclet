#include "veclet/node/replica_manager.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace veclet::node {
namespace {

bool PlacementsEqual(const veclet::v1::ShardPlacement &lhs,
                     const veclet::v1::ShardPlacement &rhs) {
  return lhs.collection_id() == rhs.collection_id() &&
         lhs.generation_id() == rhs.generation_id() &&
         lhs.shard_id() == rhs.shard_id() &&
         lhs.placement_epoch() == rhs.placement_epoch();
}

void ValidatePlacement(const veclet::v1::ShardPlacement &placement) {
  const std::string &collection_id = placement.collection_id();
  if (collection_id.empty() || collection_id.size() > 63 ||
      collection_id.front() < 'a' || collection_id.front() > 'z' ||
      !std::all_of(collection_id.begin() + 1, collection_id.end(),
                   [](unsigned char value) {
                     return (value >= 'a' && value <= 'z') ||
                            (value >= '0' && value <= '9') || value == '-';
                   })) {
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

bool RequestsEqual(const artifact::ReplicaLoadRequest &lhs,
                   const artifact::ReplicaLoadRequest &rhs) {
  return PlacementsEqual(lhs.placement, rhs.placement) &&
         lhs.manifest == rhs.manifest &&
         lhs.artifact_relative_directory == rhs.artifact_relative_directory;
}

std::string PlacementContext(const veclet::v1::ShardPlacement &placement) {
  return "collection_id=" + placement.collection_id() +
         " shard_id=" + std::to_string(placement.shard_id()) +
         " generation_id=" + std::to_string(placement.generation_id()) +
         " placement_epoch=" + std::to_string(placement.placement_epoch());
}

} // namespace

struct ReplicaManager::Slot {
  struct Operation {
    std::mutex activation_mutex;
    std::stop_source cancellation;
  };

  struct Desired {
    artifact::ReplicaLoadRequest request;
    ReplicaState state{ReplicaState::kRecovering};
    std::string failure;
    std::shared_ptr<Operation> operation;
  };

  struct Active {
    veclet::v1::ShardPlacement placement;
    std::string manifest_id;
    std::shared_ptr<shard::LocalShard> shard;
  };

  std::optional<Desired> desired;
  std::optional<Active> active;
};

ReplicaManager::ReplicaManager(DataNodeService &service,
                               std::unique_ptr<artifact::ReplicaLoader> loader)
    : service_(service), loader_(std::move(loader)) {
  if (!loader_) {
    throw std::invalid_argument("ReplicaLoader pointer must not be null");
  }
}

ReplicaManager::~ReplicaManager() {
  std::vector<std::shared_ptr<Slot::Operation>> operations;
  {
    std::lock_guard lock(mutex_);
    for (auto &entry : slots_) {
      if (entry.second->desired && entry.second->desired->operation) {
        operations.push_back(entry.second->desired->operation);
      }
    }
  }
  for (const auto &operation : operations) {
    operation->cancellation.request_stop();
  }
}

std::string ReplicaManager::MakeKey(const std::string &collection_id,
                                    uint32_t shard_id) {
  return collection_id + ':' + std::to_string(shard_id);
}

PreparePlacementOutcome
ReplicaManager::PreparePlacement(const artifact::ReplicaLoadRequest &request,
                                 std::stop_token stop_token) {
  artifact::ValidateReplicaLoadRequest(request);
  if (stop_token.stop_requested()) {
    return PreparePlacementOutcome::kCancelled;
  }
  const std::string key =
      MakeKey(request.placement.collection_id(), request.placement.shard_id());
  std::shared_ptr<Slot::Operation> operation;
  std::shared_ptr<Slot::Operation> superseded_operation;

  {
    std::lock_guard lock(mutex_);
    std::unique_ptr<Slot> &slot_pointer = slots_[key];
    if (!slot_pointer) {
      slot_pointer = std::make_unique<Slot>();
    }
    Slot &slot = *slot_pointer;

    if (slot.desired) {
      const uint64_t current_epoch =
          slot.desired->request.placement.placement_epoch();
      if (request.placement.placement_epoch() < current_epoch) {
        throw std::domain_error(
            "stale placement cannot replace newer desired state");
      }
      if (request.placement.placement_epoch() == current_epoch) {
        if (!RequestsEqual(request, slot.desired->request)) {
          throw std::domain_error(
              "placement epoch conflicts with desired replica state");
        }
        if (slot.desired->state == ReplicaState::kReady) {
          return PreparePlacementOutcome::kAlreadyReady;
        }
        if (slot.desired->state == ReplicaState::kRecovering) {
          return PreparePlacementOutcome::kAlreadyRecovering;
        }
      } else {
        if (request.placement.generation_id() <
            slot.desired->request.placement.generation_id()) {
          throw std::domain_error(
              "newer placement epoch cannot move generation backward");
        }
        if (request.placement.generation_id() ==
                slot.desired->request.placement.generation_id() &&
            request.manifest.manifest_id !=
                slot.desired->request.manifest.manifest_id) {
          throw std::domain_error(
              "immutable generation cannot change artifact identity");
        }
        if (slot.desired->operation) {
          superseded_operation = slot.desired->operation;
        }
      }
    }

    if (slot.active) {
      if (request.placement.placement_epoch() <
          slot.active->placement.placement_epoch()) {
        throw std::domain_error(
            "stale placement cannot replace active replica state");
      }
      if (request.placement.generation_id() <
          slot.active->placement.generation_id()) {
        throw std::domain_error(
            "newer placement epoch cannot move generation backward");
      }
      if (request.placement.generation_id() ==
              slot.active->placement.generation_id() &&
          request.manifest.manifest_id != slot.active->manifest_id) {
        throw std::domain_error(
            "immutable generation cannot change artifact identity");
      }
      if (request.placement.generation_id() ==
              slot.active->placement.generation_id() &&
          request.manifest.manifest_id == slot.active->manifest_id) {
        service_.InstallPlacement(request.placement, slot.active->shard);
        slot.active->placement = request.placement;
        slot.desired = Slot::Desired{
            .request = request,
            .state = ReplicaState::kReady,
            .failure = {},
            .operation = nullptr,
        };
        return PreparePlacementOutcome::kReady;
      }
    }

    operation = std::make_shared<Slot::Operation>();
    slot.desired = Slot::Desired{
        .request = request,
        .state = ReplicaState::kRecovering,
        .failure = {},
        .operation = operation,
    };
  }

  if (superseded_operation) {
    superseded_operation->cancellation.request_stop();
  }

  std::stop_callback cancel_operation(stop_token, [operation] {
    std::lock_guard activation_lock(operation->activation_mutex);
    operation->cancellation.request_stop();
  });
  std::shared_ptr<shard::LocalShard> loaded_shard;
  try {
    loaded_shard = loader_->Load(request, operation->cancellation.get_token());
    if (!loaded_shard) {
      throw std::runtime_error("ReplicaLoader returned a null LocalShard");
    }
  } catch (const artifact::ReplicaLoadCancelled &) {
    std::lock_guard lock(mutex_);
    const auto slot = slots_.find(key);
    if (slot == slots_.end() || !slot->second->desired ||
        slot->second->desired->operation != operation) {
      return PreparePlacementOutcome::kSuperseded;
    }
    if (stop_token.stop_requested()) {
      slot->second->desired->state = ReplicaState::kFailed;
      slot->second->desired->failure =
          "replica preparation was cancelled by its caller";
      return PreparePlacementOutcome::kCancelled;
    }
    slot->second->desired->state = ReplicaState::kFailed;
    slot->second->desired->failure =
        "artifact loader cancelled without a superseding placement";
    throw;
  } catch (const std::exception &error) {
    std::lock_guard lock(mutex_);
    const auto slot = slots_.find(key);
    if (slot == slots_.end() || !slot->second->desired ||
        slot->second->desired->operation != operation) {
      return PreparePlacementOutcome::kSuperseded;
    }
    if (stop_token.stop_requested()) {
      slot->second->desired->state = ReplicaState::kFailed;
      slot->second->desired->failure =
          "replica preparation was cancelled by its caller";
      return PreparePlacementOutcome::kCancelled;
    }
    slot->second->desired->state = ReplicaState::kFailed;
    slot->second->desired->failure = error.what();
    std::throw_with_nested(
        std::runtime_error("replica preparation failed for " +
                           PlacementContext(request.placement)));
  } catch (...) {
    std::lock_guard lock(mutex_);
    const auto slot = slots_.find(key);
    if (slot == slots_.end() || !slot->second->desired ||
        slot->second->desired->operation != operation) {
      return PreparePlacementOutcome::kSuperseded;
    }
    if (stop_token.stop_requested()) {
      slot->second->desired->state = ReplicaState::kFailed;
      slot->second->desired->failure =
          "replica preparation was cancelled by its caller";
      return PreparePlacementOutcome::kCancelled;
    }
    slot->second->desired->state = ReplicaState::kFailed;
    slot->second->desired->failure = "non-standard artifact loader failure";
    throw std::runtime_error("non-standard replica preparation failure for " +
                             PlacementContext(request.placement));
  }

  std::shared_ptr<shard::LocalShard> prior_shard;
  {
    std::lock_guard lock(mutex_);
    const auto slot_entry = slots_.find(key);
    if (slot_entry == slots_.end() || !slot_entry->second->desired ||
        slot_entry->second->desired->operation != operation) {
      return PreparePlacementOutcome::kSuperseded;
    }
    std::lock_guard activation_lock(operation->activation_mutex);
    Slot &slot = *slot_entry->second;
    if (stop_token.stop_requested()) {
      slot.desired->state = ReplicaState::kFailed;
      slot.desired->failure = "replica preparation was cancelled by its caller";
      return PreparePlacementOutcome::kCancelled;
    }
    prior_shard = slot.active ? slot.active->shard : nullptr;
    try {
      service_.InstallPlacement(request.placement, loaded_shard);
    } catch (const std::exception &error) {
      slot.desired->state = ReplicaState::kFailed;
      slot.desired->failure = error.what();
      std::throw_with_nested(
          std::runtime_error("replica activation failed for " +
                             PlacementContext(request.placement)));
    }
    slot.active = Slot::Active{
        .placement = request.placement,
        .manifest_id = request.manifest.manifest_id,
        .shard = loaded_shard,
    };
    slot.desired->state = ReplicaState::kReady;
    slot.desired->failure.clear();
    slot.desired->operation.reset();
  }
  return PreparePlacementOutcome::kReady;
}

bool ReplicaManager::RemovePlacement(
    const veclet::v1::ShardPlacement &placement) {
  ValidatePlacement(placement);

  const std::string key =
      MakeKey(placement.collection_id(), placement.shard_id());
  std::shared_ptr<Slot::Operation> cancelled_operation;
  std::shared_ptr<shard::LocalShard> release_after_unlock;
  bool matched = false;
  {
    std::lock_guard lock(mutex_);
    const auto slot_entry = slots_.find(key);
    if (slot_entry == slots_.end()) {
      return false;
    }
    Slot &slot = *slot_entry->second;
    if (slot.desired &&
        PlacementsEqual(slot.desired->request.placement, placement)) {
      if (slot.desired->operation) {
        cancelled_operation = slot.desired->operation;
      }
      slot.desired.reset();
      matched = true;
    }
    if (slot.active && PlacementsEqual(slot.active->placement, placement)) {
      if (!service_.RemovePlacement(placement)) {
        throw std::logic_error(
            "ReplicaManager active placement is missing from ShardRegistry");
      }
      release_after_unlock = std::move(slot.active->shard);
      slot.active.reset();
      matched = true;
    }
    if (!slot.desired && !slot.active) {
      slots_.erase(slot_entry);
    }
  }
  if (cancelled_operation) {
    cancelled_operation->cancellation.request_stop();
  }
  return matched;
}

std::optional<ReplicaStatus>
ReplicaManager::Status(const std::string &collection_id,
                       uint32_t shard_id) const {
  veclet::v1::ShardPlacement validation;
  validation.set_collection_id(collection_id);
  validation.set_generation_id(1);
  validation.set_shard_id(shard_id);
  validation.set_placement_epoch(1);
  ValidatePlacement(validation);
  const std::string key = MakeKey(collection_id, shard_id);
  std::lock_guard lock(mutex_);
  const auto slot_entry = slots_.find(key);
  if (slot_entry == slots_.end()) {
    return std::nullopt;
  }
  const Slot &slot = *slot_entry->second;
  if (slot.desired) {
    return ReplicaStatus{
        .state = slot.desired->state,
        .desired_placement = slot.desired->request.placement,
        .manifest_id = slot.desired->request.manifest.manifest_id,
        .active_placement =
            slot.active ? std::optional(slot.active->placement) : std::nullopt,
        .failure = slot.desired->failure,
    };
  }
  if (slot.active) {
    return ReplicaStatus{
        .state = ReplicaState::kReady,
        .desired_placement = slot.active->placement,
        .manifest_id = slot.active->manifest_id,
        .active_placement = slot.active->placement,
        .failure = {},
    };
  }
  return std::nullopt;
}

} // namespace veclet::node
