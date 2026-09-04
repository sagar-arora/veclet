#ifndef VECLET_NODE_STANDALONE_DATA_NODE_H_
#define VECLET_NODE_STANDALONE_DATA_NODE_H_

#include "veclet/index/local_index.h"
#include "veclet/node/data_node.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace grpc {
class Server;
} // namespace grpc

namespace veclet::node {

struct StandaloneDataNodeConfig {
  std::string listen_address{"127.0.0.1:7402"};
  std::string data_directory;
  std::string collection_id;
  uint32_t shard_id{0};
  uint64_t generation_id{1};
  uint64_t placement_epoch{1};
  int dimension{0};
  index::MetricType metric{index::MetricType::kL2};
  int hnsw_m{32};
  int hnsw_ef_search{64};
};

// Parses dependency-free --name=value arguments. The argv[0] executable name
// is not part of args. Missing required values and duplicate or unknown flags
// are rejected.
StandaloneDataNodeConfig
ParseStandaloneDataNodeArgs(std::span<const std::string_view> args);
std::string StandaloneDataNodeUsage();

class StandaloneDataNode {
public:
  // Port zero is reserved for tests that need an OS-selected loopback port.
  explicit StandaloneDataNode(StandaloneDataNodeConfig config,
                              bool allow_ephemeral_port = false);
  ~StandaloneDataNode();

  StandaloneDataNode(const StandaloneDataNode &) = delete;
  StandaloneDataNode &operator=(const StandaloneDataNode &) = delete;
  StandaloneDataNode(StandaloneDataNode &&) = delete;
  StandaloneDataNode &operator=(StandaloneDataNode &&) = delete;

  // The owner calls lifecycle methods serially from one thread. Start performs
  // no recovery: construction has already recovered the shard and installed
  // its placement. It starts the synchronous gRPC server and marks standard
  // health services as serving.
  void Start();
  void Wait();
  void Shutdown();

  const StandaloneDataNodeConfig &config() const noexcept { return config_; }
  const std::string &endpoint() const noexcept { return endpoint_; }
  int selected_port() const noexcept { return selected_port_; }

private:
  StandaloneDataNodeConfig config_;
  std::shared_ptr<shard::LocalShard> shard_;
  DataNodeService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string endpoint_;
  int selected_port_{0};
  bool start_attempted_{false};
  bool wait_completed_{false};
  bool shutdown_started_{false};
};

} // namespace veclet::node

#endif // VECLET_NODE_STANDALONE_DATA_NODE_H_
