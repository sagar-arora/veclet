#include "veclet/node/data_node.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace veclet::node {
namespace {

constexpr uint32_t kMaxSearchK = 1000;
constexpr int kMaxBatchRecords = 256;
constexpr size_t kMaxEncodedRequestBytes = 4U * 1024U * 1024U;

grpc::Status InvalidArgument(const char *message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message);
}

bool PlacementsEqual(const veclet::v1::ShardPlacement &lhs,
                     const veclet::v1::ShardPlacement &rhs) {
  return lhs.collection_id() == rhs.collection_id() &&
         lhs.generation_id() == rhs.generation_id() &&
         lhs.shard_id() == rhs.shard_id() &&
         lhs.placement_epoch() == rhs.placement_epoch();
}

std::string PlacementContext(const veclet::v1::ShardPlacement &placement) {
  return "collection_id=" + placement.collection_id() +
         " shard_id=" + std::to_string(placement.shard_id()) +
         " generation_id=" + std::to_string(placement.generation_id()) +
         " placement_epoch=" + std::to_string(placement.placement_epoch());
}

grpc::Status
ValidatePlacementFields(const veclet::v1::ShardPlacement &placement) {
  if (placement.generation_id() == 0) {
    return InvalidArgument("placement generation_id must be positive");
  }
  if (placement.placement_epoch() == 0) {
    return InvalidArgument("placement_epoch must be positive");
  }
  return grpc::Status::OK;
}

grpc::Status CancelledStatus(const grpc::ServerContext &context,
                             std::string_view rpc_name) {
  if (!context.IsCancelled()) {
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::CANCELLED,
                      std::string(rpc_name) + " was cancelled");
}

} // namespace

void DataNodeService::InstallPlacement(
    const veclet::v1::ShardPlacement &placement,
    std::shared_ptr<shard::LocalShard> shard) {
  registry_.Install(placement, std::move(shard));
}

bool DataNodeService::RemovePlacement(
    const veclet::v1::ShardPlacement &placement) {
  return registry_.Remove(placement);
}

grpc::Status DataNodeService::LookupActivePlacement(
    const veclet::v1::ShardPlacement &requested,
    ShardRegistry::PlacementHandle *placement) const {
  try {
    *placement =
        registry_.Lookup(requested.collection_id(), requested.shard_id());
  } catch (const std::invalid_argument &error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  }
  if (!*placement) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND,
                        "shard placement is not installed on this DataNode: " +
                            PlacementContext(requested));
  }
  if (!PlacementsEqual(requested, (*placement)->placement()) ||
      !(*placement)->active()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "shard placement is stale: requested " +
                            PlacementContext(requested) + " current " +
                            PlacementContext((*placement)->placement()));
  }
  return grpc::Status::OK;
}

grpc::Status
DataNodeService::SearchShard(grpc::ServerContext *context,
                             const veclet::v1::SearchShardRequest *request,
                             veclet::v1::SearchShardResponse *response) {
  try {
    return SearchShardImpl(context, request, response);
  } catch (const std::invalid_argument &error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const std::exception &error) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        std::string("unexpected SearchShard failure: ") +
                            error.what());
  } catch (...) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "unexpected non-standard SearchShard failure");
  }
}

grpc::Status
DataNodeService::SearchShardImpl(grpc::ServerContext *context,
                                 const veclet::v1::SearchShardRequest *request,
                                 veclet::v1::SearchShardResponse *response) {
  const grpc::Status initial_cancellation =
      CancelledStatus(*context, "SearchShard");
  if (!initial_cancellation.ok()) {
    return initial_cancellation;
  }
  if (request->ByteSizeLong() > kMaxEncodedRequestBytes) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "SearchShard request exceeds 4 MiB");
  }
  if (!request->has_placement()) {
    return InvalidArgument("placement is required");
  }

  const grpc::Status placement_fields =
      ValidatePlacementFields(request->placement());
  if (!placement_fields.ok()) {
    return placement_fields;
  }
  if (request->k() == 0 || request->k() > kMaxSearchK) {
    return InvalidArgument("k must be between 1 and 1000");
  }

  ShardRegistry::PlacementHandle placement;
  const grpc::Status lookup =
      LookupActivePlacement(request->placement(), &placement);
  if (!lookup.ok()) {
    return lookup;
  }

  const int dimension = placement->shard()->dimension();
  if (request->query_vector_size() != dimension) {
    return InvalidArgument("query_vector must match the shard dimension");
  }
  for (const float value : request->query_vector()) {
    if (!std::isfinite(value)) {
      return InvalidArgument("query_vector values must be finite");
    }
  }
  if (placement->shard()->metric() == index::MetricType::kCosine &&
      std::all_of(request->query_vector().begin(),
                  request->query_vector().end(),
                  [](float value) { return value == 0.0F; })) {
    return InvalidArgument("cosine query_vector must have a non-zero norm");
  }

  shard::ShardSearchResult result;
  try {
    result = placement->shard()->Search(
        std::span<const float>(request->query_vector().data(),
                               static_cast<size_t>(dimension)),
        static_cast<int>(request->k()));
  } catch (const std::invalid_argument &error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const std::exception &error) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "SearchShard failed for " +
                            PlacementContext(placement->placement()) + ": " +
                            error.what());
  }

  const grpc::Status final_cancellation =
      CancelledStatus(*context, "SearchShard");
  if (!final_cancellation.ok()) {
    return final_cancellation;
  }
  if (!placement->active()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "shard placement changed during search: " +
                            PlacementContext(placement->placement()));
  }

  response->clear_neighbors();
  for (const shard::ShardSearchHit &hit : result.hits) {
    veclet::v1::Neighbor *neighbor = response->add_neighbors();
    neighbor->set_vector_id(hit.record.vector_id());
    neighbor->set_score(hit.score);
  }
  *response->mutable_placement() = placement->placement();
  return grpc::Status::OK;
}

grpc::Status
DataNodeService::BatchInsert(grpc::ServerContext *context,
                             const veclet::v1::BatchInsertRequest *request,
                             veclet::v1::BatchInsertResponse *response) {
  try {
    return BatchInsertImpl(context, request, response);
  } catch (const std::invalid_argument &error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const std::domain_error &error) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, error.what());
  } catch (const std::exception &error) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        std::string("unexpected BatchInsert failure: ") +
                            error.what());
  } catch (...) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "unexpected non-standard BatchInsert failure");
  }
}

grpc::Status
DataNodeService::BatchInsertImpl(grpc::ServerContext *context,
                                 const veclet::v1::BatchInsertRequest *request,
                                 veclet::v1::BatchInsertResponse *response) {
  const grpc::Status initial_cancellation =
      CancelledStatus(*context, "BatchInsert");
  if (!initial_cancellation.ok()) {
    return initial_cancellation;
  }
  if (request->ByteSizeLong() > kMaxEncodedRequestBytes) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "BatchInsert request exceeds 4 MiB");
  }
  if (!request->has_placement()) {
    return InvalidArgument("placement is required");
  }

  const grpc::Status placement_fields =
      ValidatePlacementFields(request->placement());
  if (!placement_fields.ok()) {
    return placement_fields;
  }
  if (request->records_size() == 0) {
    return InvalidArgument("records must not be empty");
  }
  if (request->records_size() > kMaxBatchRecords) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "records must contain at most 256 entries");
  }

  ShardRegistry::PlacementHandle placement;
  const grpc::Status lookup =
      LookupActivePlacement(request->placement(), &placement);
  if (!lookup.ok()) {
    return lookup;
  }

  std::vector<veclet::v1::VectorRecord> records;
  try {
    records.reserve(static_cast<size_t>(request->records_size()));
    for (const veclet::v1::VectorRecord &record : request->records()) {
      records.push_back(record);
    }
  } catch (const std::bad_alloc &) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "insufficient memory to validate BatchInsert request");
  }

  const grpc::Status pre_mutation_cancellation =
      CancelledStatus(*context, "BatchInsert");
  if (!pre_mutation_cancellation.ok()) {
    return pre_mutation_cancellation;
  }
  if (!placement->active()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "shard placement changed before insert: " +
                            PlacementContext(placement->placement()));
  }

  shard::ShardPutBatchResult result;
  try {
    result = placement->shard()->PutBatch(records);
  } catch (const std::invalid_argument &error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const std::domain_error &error) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "BatchInsert conflict for " +
                            PlacementContext(placement->placement()) + ": " +
                            error.what());
  } catch (const std::exception &error) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "BatchInsert failed for " +
                            PlacementContext(placement->placement()) + ": " +
                            error.what());
  }

  const grpc::Status final_cancellation =
      CancelledStatus(*context, "BatchInsert");
  if (!final_cancellation.ok()) {
    return final_cancellation;
  }
  if (!placement->active()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "shard placement changed during insert: " +
                            PlacementContext(placement->placement()));
  }
  const size_t record_count = records.size();
  if (result.inserted_records > record_count ||
      result.duplicate_records > record_count - result.inserted_records ||
      result.inserted_records + result.duplicate_records != record_count) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "BatchInsert returned inconsistent counts for " +
                            PlacementContext(placement->placement()));
  }

  *response->mutable_placement() = placement->placement();
  response->set_inserted_records(
      static_cast<uint32_t>(result.inserted_records));
  response->set_duplicate_records(
      static_cast<uint32_t>(result.duplicate_records));
  return grpc::Status::OK;
}

} // namespace veclet::node
