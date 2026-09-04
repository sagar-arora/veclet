#include "veclet/artifact/replica_artifact.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace veclet::artifact {
namespace {

constexpr size_t kSha256HexLength = 64;
constexpr size_t kMaxManifestFiles = 100000;
constexpr size_t kMaxRelativePathBytes = 1024;
constexpr int kMaxDimension = 65536;
constexpr int kMaxHnswParameter = 65536;

bool IsValidCollectionId(std::string_view collection_id) {
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

bool IsLowercaseSha256(std::string_view value) {
  return value.size() == kSha256HexLength &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool IsDirectRocksDbFile(std::string_view relative_path) {
  if (relative_path.empty() || relative_path.size() > kMaxRelativePathBytes ||
      !relative_path.starts_with("rocksdb/") ||
      relative_path.size() == std::string_view("rocksdb/").size()) {
    return false;
  }
  const std::string_view filename =
      relative_path.substr(std::string_view("rocksdb/").size());
  return filename != "." && filename != ".." &&
         filename.find('/') == std::string_view::npos &&
         filename.find('\\') == std::string_view::npos &&
         filename.find('\0') == std::string_view::npos;
}

} // namespace

void ValidateReplicaLoadRequest(const ReplicaLoadRequest &request) {
  const veclet::v1::ShardPlacement &placement = request.placement;
  const ShardArtifactManifest &manifest = request.manifest;
  if (!IsValidCollectionId(placement.collection_id())) {
    throw std::invalid_argument(
        "placement collection_id must match [a-z][a-z0-9-]{0,62}");
  }
  if (placement.generation_id() == 0) {
    throw std::invalid_argument("placement generation_id must be positive");
  }
  if (placement.placement_epoch() == 0) {
    throw std::invalid_argument("placement_epoch must be positive");
  }
  if (manifest.collection_id != placement.collection_id() ||
      manifest.generation_id != placement.generation_id() ||
      manifest.shard_id != placement.shard_id()) {
    throw std::invalid_argument(
        "artifact manifest identity must match the shard placement");
  }
  if (!IsLowercaseSha256(manifest.manifest_id)) {
    throw std::invalid_argument(
        "manifest_id must be a lowercase SHA-256 value");
  }
  if (manifest.dimension <= 0 || manifest.dimension > kMaxDimension) {
    throw std::invalid_argument(
        "artifact dimension must be between 1 and 65536");
  }
  switch (manifest.metric) {
  case index::MetricType::kL2:
  case index::MetricType::kInnerProduct:
  case index::MetricType::kCosine:
    break;
  default:
    throw std::invalid_argument("artifact metric is unsupported");
  }
  if (manifest.hnsw_m <= 0 || manifest.hnsw_m > kMaxHnswParameter ||
      manifest.hnsw_ef_search <= 0 ||
      manifest.hnsw_ef_search > kMaxHnswParameter) {
    throw std::invalid_argument(
        "artifact HNSW parameters must be between 1 and 65536");
  }
  if (manifest.files.empty() || manifest.files.size() > kMaxManifestFiles) {
    throw std::invalid_argument(
        "artifact manifest must contain between 1 and 100000 files");
  }
  if (request.artifact_relative_directory.empty() ||
      request.artifact_relative_directory.size() > kMaxRelativePathBytes ||
      request.artifact_relative_directory.find('\0') != std::string::npos) {
    throw std::invalid_argument(
        "artifact relative directory must be between 1 and 1024 bytes");
  }

  std::string_view previous_path;
  uint64_t total_size = 0;
  for (const ArtifactFile &file : manifest.files) {
    if (!IsDirectRocksDbFile(file.relative_path)) {
      throw std::invalid_argument(
          "artifact files must be direct children of rocksdb/");
    }
    if (!previous_path.empty() && file.relative_path <= previous_path) {
      throw std::invalid_argument(
          "artifact file paths must be unique and lexicographically sorted");
    }
    previous_path = file.relative_path;
    if (!IsLowercaseSha256(file.sha256)) {
      throw std::invalid_argument(
          "artifact file checksum must be lowercase SHA-256");
    }
    if (file.size_bytes > std::numeric_limits<uint64_t>::max() - total_size) {
      throw std::invalid_argument("artifact total byte size overflows uint64");
    }
    total_size += file.size_bytes;
  }
}

ReplicaLoadCancelled::ReplicaLoadCancelled()
    : std::runtime_error("replica artifact loading was cancelled") {}

} // namespace veclet::artifact
