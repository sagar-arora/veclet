#ifndef VECLET_NODE_DATA_NODE_H_
#define VECLET_NODE_DATA_NODE_H_

#include "veclet/node/shard_registry.h"
#include "veclet/v1/data.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <memory>

namespace veclet::node {

class DataNodeService final : public veclet::v1::DataService::Service {
public:
  DataNodeService() = default;
  ~DataNodeService() override = default;

  // The owner must shut down its gRPC server and wait for synchronous RPCs to
  // finish before destroying this service. The service starts no background
  // threads of its own.

  DataNodeService(const DataNodeService &) = delete;
  DataNodeService &operator=(const DataNodeService &) = delete;
  DataNodeService(DataNodeService &&) = delete;
  DataNodeService &operator=(DataNodeService &&) = delete;

  // These in-process methods are the bootstrap/test boundary for the future
  // authenticated controller placement stream. Install requires a READY shard;
  // Remove affects only the exact current placement.
  void InstallPlacement(const veclet::v1::ShardPlacement &placement,
                        std::shared_ptr<shard::LocalShard> shard);
  bool RemovePlacement(const veclet::v1::ShardPlacement &placement);

  grpc::Status SearchShard(grpc::ServerContext *context,
                           const veclet::v1::SearchShardRequest *request,
                           veclet::v1::SearchShardResponse *response) override;

private:
  grpc::Status SearchShardImpl(grpc::ServerContext *context,
                               const veclet::v1::SearchShardRequest *request,
                               veclet::v1::SearchShardResponse *response);

  ShardRegistry registry_;
};

} // namespace veclet::node

#endif // VECLET_NODE_DATA_NODE_H_
