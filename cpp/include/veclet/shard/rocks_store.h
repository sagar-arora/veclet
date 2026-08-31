#ifndef VECLET_SHARD_ROCKS_STORE_H_
#define VECLET_SHARD_ROCKS_STORE_H_

#include "veclet/storage/v1/record.pb.h"
#include "veclet/v1/common.pb.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rocksdb {
class ColumnFamilyHandle;
class TransactionDB;
} // namespace rocksdb

namespace veclet::shard {

class RocksStore {
public:
  struct PutResult {
    veclet::storage::v1::StoredRecord stored_record;
    bool inserted{false};
  };

  explicit RocksStore(const std::string &db_path);
  ~RocksStore();

  // Non-copyable, non-movable (owns RocksDB handle)
  RocksStore(const RocksStore &) = delete;
  RocksStore &operator=(const RocksStore &) = delete;
  RocksStore(RocksStore &&) = delete;
  RocksStore &operator=(RocksStore &&) = delete;

  const std::string &db_path() const { return db_path_; }

  // RocksStore is safe for concurrent calls. Put atomically allocates the
  // positive local label and writes both mappings. An exact replay returns the
  // original label; a conflicting insert is rejected.
  PutResult Put(const veclet::v1::VectorRecord &record);
  bool Get(const std::string &vector_id,
           veclet::storage::v1::StoredRecord *stored_record) const;
  bool GetVectorIdByLocalIndexId(int64_t local_index_id,
                                 std::string *vector_id) const;
  bool
  GetByLocalIndexId(int64_t local_index_id,
                    veclet::storage::v1::StoredRecord *stored_record) const;
  size_t
  Scan(const std::function<bool(const veclet::storage::v1::StoredRecord &)>
           &callback) const;
  size_t Count() const;

private:
  void InitializeMetadata();

  std::string db_path_;
  std::unique_ptr<rocksdb::TransactionDB> db_;
  rocksdb::ColumnFamilyHandle *default_cf_{nullptr};
  rocksdb::ColumnFamilyHandle *records_cf_{nullptr};
  rocksdb::ColumnFamilyHandle *index_ids_cf_{nullptr};
};

} // namespace veclet::shard

#endif // VECLET_SHARD_ROCKS_STORE_H_
