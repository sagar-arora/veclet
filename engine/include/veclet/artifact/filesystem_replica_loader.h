#ifndef VECLET_ARTIFACT_FILESYSTEM_REPLICA_LOADER_H_
#define VECLET_ARTIFACT_FILESYSTEM_REPLICA_LOADER_H_

#include "veclet/artifact/replica_artifact.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

namespace veclet::artifact {

struct FilesystemReplicaLoaderLimits {
  size_t max_files{100000};
  uint64_t max_file_bytes{128ULL * 1024ULL * 1024ULL * 1024ULL};
  uint64_t max_total_bytes{1024ULL * 1024ULL * 1024ULL * 1024ULL};
  size_t copy_buffer_bytes{1024ULL * 1024ULL};
};

// Computes a lowercase SHA-256 digest for artifact publication and tests.
// FilesystemReplicaLoader computes the same digest while copying each file.
std::string ComputeFileSha256(const std::filesystem::path &path);

class FilesystemReplicaLoader final : public ReplicaLoader {
public:
  // source_root is read-only artifact storage. data_root owns immutable caches,
  // staging directories, and writable local replica directories. The roots
  // must be distinct and must not contain one another. One loader owns a data
  // root; construction removes staging directories abandoned by a prior
  // process. Concurrent Load calls on that loader remain supported.
  FilesystemReplicaLoader(
      std::filesystem::path source_root, std::filesystem::path data_root,
      FilesystemReplicaLoaderLimits limits = FilesystemReplicaLoaderLimits{});

  // Thread-safe. Calls use unique staging paths and check stop_token between
  // bounded copy chunks. This class starts no background threads.
  std::shared_ptr<shard::LocalShard> Load(const ReplicaLoadRequest &request,
                                          std::stop_token stop_token) override;

private:
  std::filesystem::path source_root_;
  std::filesystem::path data_root_;
  FilesystemReplicaLoaderLimits limits_;
  std::atomic<uint64_t> next_staging_id_{1};
};

} // namespace veclet::artifact

#endif // VECLET_ARTIFACT_FILESYSTEM_REPLICA_LOADER_H_
