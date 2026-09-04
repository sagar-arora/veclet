#include "veclet/node/standalone_data_node.h"

#include "veclet/index/hnsw_flat_index.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace veclet::node {
namespace {

constexpr int kMaxMessageBytes = 4 * 1024 * 1024;
constexpr uint64_t kMaxDimension = 65536;
constexpr uint64_t kMaxHnswParameter = 65536;
constexpr std::string_view kDataServiceName = "veclet.v1.DataService";

uint64_t ParseUnsigned(std::string_view value, std::string_view flag_name,
                       uint64_t maximum) {
  uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      parsed > maximum) {
    throw std::invalid_argument("--" + std::string(flag_name) +
                                " must be an unsigned integer between 0 and " +
                                std::to_string(maximum));
  }
  return parsed;
}

void ValidateCollectionId(std::string_view collection_id) {
  if (collection_id.empty() || collection_id.size() > 63 ||
      collection_id.front() < 'a' || collection_id.front() > 'z') {
    throw std::invalid_argument(
        "--collection-id must match [a-z][a-z0-9-]{0,62}");
  }
  for (const char character : collection_id.substr(1)) {
    const bool lowercase = character >= 'a' && character <= 'z';
    const bool digit = character >= '0' && character <= '9';
    if (!lowercase && !digit && character != '-') {
      throw std::invalid_argument(
          "--collection-id must match [a-z][a-z0-9-]{0,62}");
    }
  }
}

uint16_t ValidateListenAddress(std::string_view listen_address,
                               bool allow_ephemeral_port) {
  const size_t separator = listen_address.rfind(':');
  if (separator == std::string_view::npos) {
    throw std::invalid_argument(
        "--listen-address must be 127.0.0.1:<port> or localhost:<port>");
  }
  const std::string_view host = listen_address.substr(0, separator);
  if (host != "127.0.0.1" && host != "localhost") {
    throw std::invalid_argument(
        "--listen-address must use loopback host 127.0.0.1 or localhost");
  }
  const uint64_t port = ParseUnsigned(listen_address.substr(separator + 1),
                                      "listen-address port", 65535);
  if (port == 0 && !allow_ephemeral_port) {
    throw std::invalid_argument(
        "--listen-address port must be between 1 and 65535");
  }
  return static_cast<uint16_t>(port);
}

void ValidateConfig(const StandaloneDataNodeConfig &config,
                    bool allow_ephemeral_port) {
  if (config.data_directory.empty() ||
      config.data_directory.find('\0') != std::string::npos) {
    throw std::invalid_argument("--data-directory must not be empty");
  }
  ValidateCollectionId(config.collection_id);
  if (config.dimension <= 0 ||
      static_cast<uint64_t>(config.dimension) > kMaxDimension) {
    throw std::invalid_argument("--dimension must be between 1 and 65536");
  }
  switch (config.metric) {
  case index::MetricType::kL2:
  case index::MetricType::kInnerProduct:
  case index::MetricType::kCosine:
    break;
  default:
    throw std::invalid_argument(
        "--metric must be l2, inner-product, or cosine");
  }
  if (config.generation_id == 0) {
    throw std::invalid_argument("--generation-id must be positive");
  }
  if (config.placement_epoch == 0) {
    throw std::invalid_argument("--placement-epoch must be positive");
  }
  if (config.hnsw_m <= 0 ||
      static_cast<uint64_t>(config.hnsw_m) > kMaxHnswParameter) {
    throw std::invalid_argument("--hnsw-m must be between 1 and 65536");
  }
  if (config.hnsw_ef_search <= 0 ||
      static_cast<uint64_t>(config.hnsw_ef_search) > kMaxHnswParameter) {
    throw std::invalid_argument("--hnsw-ef-search must be between 1 and 65536");
  }
  static_cast<void>(
      ValidateListenAddress(config.listen_address, allow_ephemeral_port));
}

std::string EndpointWithSelectedPort(std::string_view listen_address,
                                     int selected_port) {
  const size_t separator = listen_address.rfind(':');
  return std::string(listen_address.substr(0, separator + 1)) +
         std::to_string(selected_port);
}

std::string MetricDirectoryName(index::MetricType metric) {
  switch (metric) {
  case index::MetricType::kL2:
    return "l2";
  case index::MetricType::kInnerProduct:
    return "inner-product";
  case index::MetricType::kCosine:
    return "cosine";
  }
  throw std::invalid_argument("unsupported metric type");
}

std::filesystem::path
LocalDatabasePath(const StandaloneDataNodeConfig &config) {
  return std::filesystem::path(config.data_directory) / "collections" /
         config.collection_id /
         ("generation-" + std::to_string(config.generation_id)) /
         ("shard-" + std::to_string(config.shard_id)) /
         ("dimension-" + std::to_string(config.dimension)) /
         ("metric-" + MetricDirectoryName(config.metric)) / "rocksdb";
}

void EnableHealthServiceOnce() {
  static std::once_flag enabled;
  std::call_once(enabled, [] { grpc::EnableDefaultHealthCheckService(true); });
}

} // namespace

StandaloneDataNodeConfig
ParseStandaloneDataNodeArgs(std::span<const std::string_view> args) {
  StandaloneDataNodeConfig config;
  std::unordered_set<std::string> seen;
  bool has_data_directory = false;
  bool has_collection_id = false;
  bool has_dimension = false;
  bool has_metric = false;

  for (const std::string_view argument : args) {
    if (!argument.starts_with("--")) {
      throw std::invalid_argument(
          "standalone DataNode accepts only --name=value arguments");
    }
    const size_t separator = argument.find('=');
    if (separator == std::string_view::npos || separator == 2 ||
        separator + 1 == argument.size()) {
      throw std::invalid_argument("argument must use non-empty --name=value");
    }

    const std::string name(argument.substr(2, separator - 2));
    const std::string_view value = argument.substr(separator + 1);
    if (!seen.insert(name).second) {
      throw std::invalid_argument("duplicate flag --" + name);
    }

    if (name == "data-directory") {
      config.data_directory = value;
      has_data_directory = true;
    } else if (name == "collection-id") {
      config.collection_id = value;
      has_collection_id = true;
    } else if (name == "dimension") {
      config.dimension =
          static_cast<int>(ParseUnsigned(value, name, kMaxDimension));
      has_dimension = true;
    } else if (name == "metric") {
      if (value == "l2") {
        config.metric = index::MetricType::kL2;
      } else if (value == "inner-product") {
        config.metric = index::MetricType::kInnerProduct;
      } else if (value == "cosine") {
        config.metric = index::MetricType::kCosine;
      } else {
        throw std::invalid_argument(
            "--metric must be l2, inner-product, or cosine");
      }
      has_metric = true;
    } else if (name == "listen-address") {
      config.listen_address = value;
    } else if (name == "shard-id") {
      config.shard_id = static_cast<uint32_t>(
          ParseUnsigned(value, name, std::numeric_limits<uint32_t>::max()));
    } else if (name == "generation-id") {
      config.generation_id =
          ParseUnsigned(value, name, std::numeric_limits<uint64_t>::max());
    } else if (name == "placement-epoch") {
      config.placement_epoch =
          ParseUnsigned(value, name, std::numeric_limits<uint64_t>::max());
    } else if (name == "index") {
      if (value != "hnsw") {
        throw std::invalid_argument("--index must be hnsw");
      }
    } else if (name == "hnsw-m") {
      config.hnsw_m =
          static_cast<int>(ParseUnsigned(value, name, kMaxHnswParameter));
    } else if (name == "hnsw-ef-search") {
      config.hnsw_ef_search =
          static_cast<int>(ParseUnsigned(value, name, kMaxHnswParameter));
    } else {
      throw std::invalid_argument("unknown flag --" + name);
    }
  }

  if (!has_data_directory) {
    throw std::invalid_argument("missing required flag --data-directory");
  }
  if (!has_collection_id) {
    throw std::invalid_argument("missing required flag --collection-id");
  }
  if (!has_dimension) {
    throw std::invalid_argument("missing required flag --dimension");
  }
  if (!has_metric) {
    throw std::invalid_argument("missing required flag --metric");
  }
  ValidateConfig(config, false);
  return config;
}

std::string StandaloneDataNodeUsage() {
  return "Usage: veclet-datad --data-directory=PATH --collection-id=ID "
         "--dimension=N --metric=l2|inner-product|cosine "
         "[--listen-address=127.0.0.1:7402] [--shard-id=0] "
         "[--generation-id=1] [--placement-epoch=1] [--index=hnsw] "
         "[--hnsw-m=32] [--hnsw-ef-search=64]\n";
}

StandaloneDataNode::StandaloneDataNode(StandaloneDataNodeConfig config,
                                       bool allow_ephemeral_port)
    : config_(std::move(config)) {
  ValidateConfig(config_, allow_ephemeral_port);
  auto index = std::make_unique<index::HnswFlatIndex>(
      config_.dimension, config_.metric, config_.hnsw_m,
      config_.hnsw_ef_search);
  const std::filesystem::path database_path = LocalDatabasePath(config_);
  std::filesystem::create_directories(database_path.parent_path());
  shard_ = std::make_shared<shard::LocalShard>(database_path.string(),
                                               std::move(index));

  veclet::v1::ShardPlacement placement;
  placement.set_collection_id(config_.collection_id);
  placement.set_generation_id(config_.generation_id);
  placement.set_shard_id(config_.shard_id);
  placement.set_placement_epoch(config_.placement_epoch);
  service_.InstallPlacement(placement, shard_);
}

StandaloneDataNode::~StandaloneDataNode() {
  Shutdown();
  if (server_ && !wait_completed_) {
    server_->Wait();
  }
}

void StandaloneDataNode::Start() {
  if (server_ || start_attempted_) {
    throw std::logic_error("standalone DataNode Start may be called only once");
  }
  if (shutdown_started_) {
    throw std::logic_error("standalone DataNode cannot start after shutdown");
  }
  start_attempted_ = true;
  EnableHealthServiceOnce();

  grpc::ServerBuilder builder;
  builder.SetMaxReceiveMessageSize(kMaxMessageBytes);
  builder.SetMaxSendMessageSize(kMaxMessageBytes);
  builder.RegisterService(&service_);

  int selected_port = 0;
  builder.AddListeningPort(config_.listen_address,
                           grpc::InsecureServerCredentials(), &selected_port);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server || selected_port <= 0) {
    if (server) {
      server->Shutdown();
      server->Wait();
    }
    throw std::runtime_error("failed to listen on " + config_.listen_address);
  }

  grpc::HealthCheckServiceInterface *health = server->GetHealthCheckService();
  if (health == nullptr) {
    server->Shutdown();
    server->Wait();
    throw std::runtime_error("standard gRPC health service is unavailable");
  }
  health->SetServingStatus("", true);
  health->SetServingStatus(std::string(kDataServiceName), true);

  selected_port_ = selected_port;
  endpoint_ = EndpointWithSelectedPort(config_.listen_address, selected_port_);
  server_ = std::move(server);
}

void StandaloneDataNode::Wait() {
  if (!server_) {
    throw std::logic_error("standalone DataNode has not started");
  }
  if (wait_completed_) {
    return;
  }
  server_->Wait();
  wait_completed_ = true;
}

void StandaloneDataNode::Shutdown() {
  if (shutdown_started_) {
    return;
  }
  shutdown_started_ = true;
  if (!server_) {
    return;
  }
  if (grpc::HealthCheckServiceInterface *health =
          server_->GetHealthCheckService()) {
    health->SetServingStatus(std::string(kDataServiceName), false);
    health->SetServingStatus("", false);
  }
  server_->Shutdown();
}

} // namespace veclet::node
