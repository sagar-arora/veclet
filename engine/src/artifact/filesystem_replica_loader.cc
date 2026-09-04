#include "veclet/artifact/filesystem_replica_loader.h"

#include "veclet/index/hnsw_flat_index.h"

#include <openssl/evp.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace veclet::artifact {
namespace {

constexpr size_t kSha256Bytes = 32;
constexpr size_t kMinimumCopyBufferBytes = 4096;
constexpr size_t kMaximumCopyBufferBytes = 16 * 1024 * 1024;
constexpr std::string_view kCompletionMarker = ".veclet-manifest-id";

class FileDescriptor {
public:
  explicit FileDescriptor(int descriptor) : descriptor_(descriptor) {}
  ~FileDescriptor() {
    if (descriptor_ >= 0) {
      static_cast<void>(close(descriptor_));
    }
  }

  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;

  int get() const noexcept { return descriptor_; }

private:
  int descriptor_;
};

struct DigestResult {
  uint64_t bytes{0};
  std::string sha256;
};

void CheckCancelled(std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw ReplicaLoadCancelled();
  }
}

bool IsWithin(const std::filesystem::path &root,
              const std::filesystem::path &candidate) {
  auto root_part = root.begin();
  auto candidate_part = candidate.begin();
  while (root_part != root.end() && candidate_part != candidate.end()) {
    if (*root_part != *candidate_part) {
      return false;
    }
    ++root_part;
    ++candidate_part;
  }
  return root_part == root.end();
}

void ValidateRelativeDirectory(std::string_view value) {
  const std::filesystem::path path(value);
  if (path.empty() || path.is_absolute() || path.has_root_name() ||
      path.has_root_directory()) {
    throw std::invalid_argument(
        "artifact directory must be a non-empty relative path");
  }
  for (const std::filesystem::path &part : path) {
    const std::string component = part.string();
    if (component.empty() || component == "." || component == ".." ||
        component.find('\\') != std::string::npos ||
        component.find('\0') != std::string::npos) {
      throw std::invalid_argument(
          "artifact directory contains an unsafe path component");
    }
  }
}

void ValidateLimits(const FilesystemReplicaLoaderLimits &limits) {
  if (limits.max_files == 0 || limits.max_files > 100000) {
    throw std::invalid_argument("max_files must be between 1 and 100000");
  }
  if (limits.max_file_bytes == 0 || limits.max_total_bytes == 0 ||
      limits.max_file_bytes > limits.max_total_bytes) {
    throw std::invalid_argument("filesystem artifact byte limits are invalid");
  }
  if (limits.copy_buffer_bytes < kMinimumCopyBufferBytes ||
      limits.copy_buffer_bytes > kMaximumCopyBufferBytes) {
    throw std::invalid_argument(
        "copy_buffer_bytes must be between 4096 and 16777216");
  }
}

std::string HexDigest(const std::array<unsigned char, kSha256Bytes> &digest) {
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (const unsigned char byte : digest) {
    result << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return result.str();
}

DigestResult DigestFile(const std::filesystem::path &source,
                        const std::optional<std::filesystem::path> &destination,
                        size_t buffer_bytes, std::stop_token stop_token) {
  CheckCancelled(stop_token);
  const std::filesystem::file_status source_status =
      std::filesystem::symlink_status(source);
  if (std::filesystem::is_symlink(source_status) ||
      !std::filesystem::is_regular_file(source_status)) {
    throw std::runtime_error("artifact path is not a regular file: " +
                             source.string());
  }

  std::ifstream input(source, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open artifact file: " +
                             source.string());
  }
  std::ofstream output;
  if (destination) {
    std::filesystem::create_directories(destination->parent_path());
    output.open(*destination, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      throw std::runtime_error("failed to create staged artifact file: " +
                               destination->string());
    }
  }

  using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context ||
      EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("failed to initialize SHA-256 verification");
  }

  std::vector<char> buffer(buffer_bytes);
  uint64_t total_bytes = 0;
  while (input) {
    CheckCancelled(stop_token);
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read_bytes = input.gcount();
    if (read_bytes <= 0) {
      continue;
    }
    const uint64_t unsigned_bytes = static_cast<uint64_t>(read_bytes);
    if (total_bytes > std::numeric_limits<uint64_t>::max() - unsigned_bytes) {
      throw std::runtime_error("artifact file size overflows uint64");
    }
    total_bytes += unsigned_bytes;
    if (EVP_DigestUpdate(context.get(), buffer.data(),
                         static_cast<size_t>(read_bytes)) != 1) {
      throw std::runtime_error("SHA-256 verification failed");
    }
    if (destination) {
      output.write(buffer.data(), read_bytes);
      if (!output) {
        throw std::runtime_error("failed while writing staged artifact file: " +
                                 destination->string());
      }
    }
  }
  if (input.bad()) {
    throw std::runtime_error("failed while reading artifact file: " +
                             source.string());
  }
  if (destination) {
    output.flush();
    output.close();
    if (!output) {
      throw std::runtime_error("failed to flush staged artifact file: " +
                               destination->string());
    }
  }

  std::array<unsigned char, kSha256Bytes> digest{};
  unsigned int digest_bytes = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_bytes) != 1 ||
      digest_bytes != digest.size()) {
    throw std::runtime_error("failed to finalize SHA-256 verification");
  }
  return {.bytes = total_bytes, .sha256 = HexDigest(digest)};
}

void SyncPath(const std::filesystem::path &path, bool directory) {
  int flags = O_RDONLY;
#ifdef O_DIRECTORY
  if (directory) {
    flags |= O_DIRECTORY;
  }
#else
  static_cast<void>(directory);
#endif
  const int raw_descriptor = open(path.c_str(), flags);
  if (raw_descriptor < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "failed to open path for fsync: " + path.string());
  }
  FileDescriptor descriptor(raw_descriptor);
  if (fsync(descriptor.get()) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "failed to fsync path: " + path.string());
  }
}

void WriteCompletionMarker(const std::filesystem::path &directory,
                           std::string_view manifest_id) {
  const std::filesystem::path marker = directory / kCompletionMarker;
  std::ofstream output(marker, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("failed to create artifact completion marker");
  }
  output << manifest_id << '\n';
  output.flush();
  output.close();
  if (!output) {
    throw std::runtime_error("failed to flush artifact completion marker");
  }
  std::filesystem::permissions(marker,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  SyncPath(marker, false);
}

void ValidateCompletionMarker(const std::filesystem::path &directory,
                              std::string_view manifest_id) {
  std::ifstream input(directory / kCompletionMarker, std::ios::binary);
  std::string stored_id;
  if (!input.is_open() || !std::getline(input, stored_id) ||
      stored_id != manifest_id) {
    throw std::runtime_error(
        "local replica completion marker does not match manifest");
  }
}

void ValidateManifestLimits(const ShardArtifactManifest &manifest,
                            const FilesystemReplicaLoaderLimits &limits) {
  if (manifest.files.size() > limits.max_files) {
    throw std::invalid_argument("artifact manifest exceeds max_files");
  }
  uint64_t total_bytes = 0;
  for (const ArtifactFile &file : manifest.files) {
    if (file.size_bytes > limits.max_file_bytes) {
      throw std::invalid_argument("artifact file exceeds max_file_bytes");
    }
    if (total_bytes > limits.max_total_bytes - file.size_bytes) {
      throw std::invalid_argument("artifact exceeds max_total_bytes");
    }
    total_bytes += file.size_bytes;
  }
}

void CopyAndVerifyManifest(const std::filesystem::path &source_directory,
                           const std::filesystem::path &target_directory,
                           const ShardArtifactManifest &manifest,
                           const FilesystemReplicaLoaderLimits &limits,
                           std::stop_token stop_token) {
  std::filesystem::create_directories(target_directory / "rocksdb");
  std::filesystem::permissions(target_directory,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  std::filesystem::permissions(target_directory / "rocksdb",
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);

  for (const ArtifactFile &file : manifest.files) {
    CheckCancelled(stop_token);
    const std::filesystem::path requested_source =
        source_directory / std::filesystem::path(file.relative_path);
    const std::filesystem::path source =
        std::filesystem::canonical(requested_source);
    if (!IsWithin(source_directory, source) ||
        std::filesystem::is_symlink(
            std::filesystem::symlink_status(requested_source))) {
      throw std::runtime_error("artifact file escapes its source directory: " +
                               file.relative_path);
    }
    const std::filesystem::path destination =
        target_directory / std::filesystem::path(file.relative_path);
    const uint64_t observed_size = std::filesystem::file_size(source);
    if (observed_size != file.size_bytes) {
      throw std::runtime_error("artifact file size mismatch: " +
                               file.relative_path);
    }
    const DigestResult digest =
        DigestFile(source, destination, limits.copy_buffer_bytes, stop_token);
    if (digest.bytes != file.size_bytes || digest.sha256 != file.sha256) {
      throw std::runtime_error("artifact file checksum mismatch: " +
                               file.relative_path);
    }
    std::filesystem::permissions(destination,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
    SyncPath(destination, false);
  }
  WriteCompletionMarker(target_directory, manifest.manifest_id);
  SyncPath(target_directory / "rocksdb", true);
  SyncPath(target_directory, true);
}

void VerifyManifestDirectory(const std::filesystem::path &directory,
                             const ShardArtifactManifest &manifest,
                             const FilesystemReplicaLoaderLimits &limits,
                             std::stop_token stop_token) {
  ValidateCompletionMarker(directory, manifest.manifest_id);
  for (const ArtifactFile &file : manifest.files) {
    CheckCancelled(stop_token);
    const std::filesystem::path path = directory / file.relative_path;
    if (std::filesystem::file_size(path) != file.size_bytes) {
      throw std::runtime_error("cached artifact file size mismatch: " +
                               file.relative_path);
    }
    const DigestResult digest =
        DigestFile(path, std::nullopt, limits.copy_buffer_bytes, stop_token);
    if (digest.bytes != file.size_bytes || digest.sha256 != file.sha256) {
      throw std::runtime_error("cached artifact checksum mismatch: " +
                               file.relative_path);
    }
  }
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
  throw std::invalid_argument("unsupported artifact metric");
}

std::filesystem::path CacheDirectory(const std::filesystem::path &data_root,
                                     const ShardArtifactManifest &manifest) {
  return data_root / "artifact-cache" / manifest.collection_id /
         ("generation-" + std::to_string(manifest.generation_id)) /
         ("shard-" + std::to_string(manifest.shard_id)) /
         ("manifest-" + manifest.manifest_id);
}

std::filesystem::path LiveDirectory(const std::filesystem::path &data_root,
                                    const ShardArtifactManifest &manifest) {
  return data_root / "replicas" / manifest.collection_id /
         ("generation-" + std::to_string(manifest.generation_id)) /
         ("shard-" + std::to_string(manifest.shard_id)) /
         ("dimension-" + std::to_string(manifest.dimension)) /
         ("metric-" + MetricDirectoryName(manifest.metric)) /
         ("manifest-" + manifest.manifest_id);
}

void RenameDirectory(const std::filesystem::path &staging,
                     const std::filesystem::path &destination) {
  std::filesystem::create_directories(destination.parent_path());
  std::error_code error;
  std::filesystem::rename(staging, destination, error);
  if (error) {
    throw std::system_error(error, "failed to activate staged replica");
  }
  SyncPath(destination.parent_path(), true);
}

[[noreturn]] void
CleanupAndRethrow(const std::filesystem::path &staging,
                  const std::exception_ptr &original_failure) {
  std::error_code cleanup_error;
  std::filesystem::remove_all(staging, cleanup_error);
  if (cleanup_error) {
    try {
      std::rethrow_exception(original_failure);
    } catch (...) {
      std::throw_with_nested(std::runtime_error(
          "replica staging cleanup failed for " + staging.string() + ": " +
          cleanup_error.message()));
    }
  }
  std::rethrow_exception(original_failure);
}

void StageAndActivate(const std::filesystem::path &source_directory,
                      const std::filesystem::path &staging_directory,
                      const std::filesystem::path &destination_directory,
                      const ShardArtifactManifest &manifest,
                      const FilesystemReplicaLoaderLimits &limits,
                      std::stop_token stop_token) {
  try {
    CopyAndVerifyManifest(source_directory, staging_directory, manifest, limits,
                          stop_token);
    CheckCancelled(stop_token);
    RenameDirectory(staging_directory, destination_directory);
  } catch (...) {
    CleanupAndRethrow(staging_directory, std::current_exception());
  }
}

std::filesystem::path
ResolveSourceDirectory(const std::filesystem::path &source_root,
                       std::string_view artifact_relative_directory) {
  ValidateRelativeDirectory(artifact_relative_directory);
  const std::filesystem::path source_directory = std::filesystem::canonical(
      source_root / std::filesystem::path(artifact_relative_directory));
  if (!IsWithin(source_root, source_directory) ||
      !std::filesystem::is_directory(source_directory)) {
    throw std::invalid_argument(
        "artifact directory escapes the configured source root");
  }
  return source_directory;
}

} // namespace

std::string ComputeFileSha256(const std::filesystem::path &path) {
  return DigestFile(path, std::nullopt, 1024 * 1024, std::stop_token{}).sha256;
}

FilesystemReplicaLoader::FilesystemReplicaLoader(
    std::filesystem::path source_root, std::filesystem::path data_root,
    FilesystemReplicaLoaderLimits limits)
    : limits_(limits) {
  ValidateLimits(limits_);
  if (source_root.empty() || data_root.empty()) {
    throw std::invalid_argument(
        "filesystem artifact source and data roots must not be empty");
  }
  source_root_ = std::filesystem::canonical(std::move(source_root));
  if (!std::filesystem::is_directory(source_root_)) {
    throw std::invalid_argument(
        "filesystem artifact source must be a directory");
  }
  const std::filesystem::path prospective_data_root =
      std::filesystem::weakly_canonical(data_root);
  if (IsWithin(source_root_, prospective_data_root) ||
      IsWithin(prospective_data_root, source_root_)) {
    throw std::invalid_argument(
        "filesystem artifact source and data roots must not overlap");
  }
  std::filesystem::create_directories(data_root);
  data_root_ = std::filesystem::canonical(std::move(data_root));
  if (IsWithin(source_root_, data_root_) ||
      IsWithin(data_root_, source_root_)) {
    throw std::invalid_argument(
        "filesystem artifact source and data roots must not overlap");
  }
  std::filesystem::permissions(data_root_, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  const std::filesystem::path staging_root = data_root_ / "staging";
  std::filesystem::remove_all(staging_root);
  std::filesystem::create_directories(staging_root);
  std::filesystem::permissions(staging_root, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  SyncPath(data_root_, true);
}

std::shared_ptr<shard::LocalShard>
FilesystemReplicaLoader::Load(const ReplicaLoadRequest &request,
                              std::stop_token stop_token) {
  ValidateReplicaLoadRequest(request);
  ValidateManifestLimits(request.manifest, limits_);
  CheckCancelled(stop_token);

  const ShardArtifactManifest &manifest = request.manifest;
  const std::filesystem::path cache_directory =
      CacheDirectory(data_root_, manifest);
  const std::filesystem::path live_directory =
      LiveDirectory(data_root_, manifest);
  bool created_live_directory = false;

  if (std::filesystem::exists(live_directory)) {
    ValidateCompletionMarker(live_directory, manifest.manifest_id);
    if (!std::filesystem::is_directory(live_directory / "rocksdb")) {
      throw std::runtime_error("local replica RocksDB directory is missing");
    }
  } else {
    if (std::filesystem::exists(cache_directory)) {
      try {
        VerifyManifestDirectory(cache_directory, manifest, limits_, stop_token);
      } catch (const ReplicaLoadCancelled &) {
        throw;
      } catch (...) {
        std::filesystem::remove_all(cache_directory);
      }
    }

    if (!std::filesystem::exists(cache_directory)) {
      const std::filesystem::path source_directory = ResolveSourceDirectory(
          source_root_, request.artifact_relative_directory);
      const uint64_t staging_id =
          next_staging_id_.fetch_add(1, std::memory_order_relaxed);
      const std::filesystem::path cache_staging =
          data_root_ / "staging" /
          ("cache-" + manifest.manifest_id + '-' + std::to_string(staging_id));
      StageAndActivate(source_directory, cache_staging, cache_directory,
                       manifest, limits_, stop_token);
    }

    const uint64_t staging_id =
        next_staging_id_.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path live_staging =
        data_root_ / "staging" /
        ("live-" + manifest.manifest_id + '-' + std::to_string(staging_id));
    StageAndActivate(cache_directory, live_staging, live_directory, manifest,
                     limits_, stop_token);
    created_live_directory = true;
  }

  CheckCancelled(stop_token);
  try {
    auto index = std::make_unique<index::HnswFlatIndex>(
        manifest.dimension, manifest.metric, manifest.hnsw_m,
        manifest.hnsw_ef_search);
    auto shard = std::make_shared<shard::LocalShard>(
        (live_directory / "rocksdb").string(), std::move(index));
    CheckCancelled(stop_token);
    return shard;
  } catch (...) {
    if (created_live_directory) {
      CleanupAndRethrow(live_directory, std::current_exception());
    }
    throw;
  }
}

} // namespace veclet::artifact
