#ifndef VECLET_ARTIFACT_REPLICA_ARTIFACT_H_
#define VECLET_ARTIFACT_REPLICA_ARTIFACT_H_

#include "veclet/index/local_index.h"
#include "veclet/shard/local_shard.h"
#include "veclet/v1/data.pb.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace veclet::artifact {

struct ArtifactFile {
  // Path relative to one artifact root. The first implementation accepts only
  // direct files under rocksdb/ and never follows symlinks.
  std::string relative_path;
  uint64_t size_bytes{0};
  std::string sha256;

  bool operator==(const ArtifactFile &) const = default;
};

struct ShardArtifactManifest {
  // Lowercase SHA-256 identity of the future serialized manifest contract.
  // This slice receives the already-parsed, trusted manifest structure and
  // verifies every file digest; serialized manifest parsing remains separate.
  std::string manifest_id;
  std::string collection_id;
  uint64_t generation_id{0};
  uint32_t shard_id{0};
  int dimension{0};
  index::MetricType metric{index::MetricType::kL2};
  int hnsw_m{32};
  int hnsw_ef_search{64};
  std::vector<ArtifactFile> files;

  bool operator==(const ShardArtifactManifest &) const = default;
};

struct ReplicaLoadRequest {
  veclet::v1::ShardPlacement placement;
  ShardArtifactManifest manifest;

  // Provider-relative artifact location. FilesystemReplicaLoader resolves this
  // beneath its configured source root. Cloud loaders will interpret an
  // equivalent provider-relative object prefix.
  std::string artifact_relative_directory;
};

void ValidateReplicaLoadRequest(const ReplicaLoadRequest &request);

class ReplicaLoadCancelled final : public std::runtime_error {
public:
  ReplicaLoadCancelled();
};

class ReplicaLoader {
public:
  virtual ~ReplicaLoader() = default;

  // Implementations support concurrent calls, perform no unbounded retry, and
  // check cancellation during bounded chunks of blocking work.
  virtual std::shared_ptr<shard::LocalShard>
  Load(const ReplicaLoadRequest &request, std::stop_token stop_token) = 0;
};

} // namespace veclet::artifact

#endif // VECLET_ARTIFACT_REPLICA_ARTIFACT_H_
