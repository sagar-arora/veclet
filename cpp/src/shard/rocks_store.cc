#include "veclet/shard/rocks_store.h"

#include "record_validation.h"

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace veclet::shard {
namespace {

constexpr uint64_t kSchemaVersion = 1;
constexpr char kSchemaVersionKeyBytes[] = "\x00veclet.schema_version";
constexpr std::string_view kSchemaVersionKey(kSchemaVersionKeyBytes,
                                             sizeof(kSchemaVersionKeyBytes) -
                                                 1);
constexpr char kNextLocalIndexIdKeyBytes[] = "\x00veclet.next_local_index_id";
constexpr std::string_view
    kNextLocalIndexIdKey(kNextLocalIndexIdKeyBytes,
                         sizeof(kNextLocalIndexIdKeyBytes) - 1);
constexpr char kRecordsColumnFamily[] = "records";
constexpr char kIndexIdsColumnFamily[] = "index_ids";

rocksdb::Slice AsSlice(std::string_view value) {
  return rocksdb::Slice(value.data(), value.size());
}

std::string EncodeUint64(uint64_t value) {
  std::string encoded(8, '\0');
  for (size_t i = 0; i < encoded.size(); ++i) {
    encoded[encoded.size() - i - 1] = static_cast<char>(value & 0xff);
    value >>= 8;
  }
  return encoded;
}

uint64_t DecodeUint64(std::string_view encoded, std::string_view field_name) {
  if (encoded.size() != 8) {
    throw std::runtime_error("Corrupt RocksDB " + std::string(field_name) +
                             ": expected eight bytes");
  }
  uint64_t value = 0;
  for (unsigned char byte : encoded) {
    value = (value << 8) | byte;
  }
  return value;
}

void CheckStatus(const rocksdb::Status &status, std::string_view operation) {
  if (!status.ok()) {
    throw std::runtime_error(std::string(operation) + ": " + status.ToString());
  }
}

veclet::storage::v1::StoredRecord ParseStoredRecord(std::string_view serialized,
                                                    std::string_view context) {
  if (serialized.size() >
      static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("Corrupt StoredRecord " + std::string(context) +
                             ": value is too large");
  }
  veclet::storage::v1::StoredRecord stored_record;
  if (!stored_record.ParseFromArray(serialized.data(),
                                    static_cast<int>(serialized.size()))) {
    throw std::runtime_error("Corrupt StoredRecord " + std::string(context) +
                             ": protobuf parse failed");
  }
  if (stored_record.local_index_id() <= 0) {
    throw std::runtime_error("Corrupt StoredRecord " + std::string(context) +
                             ": local_index_id is not positive");
  }
  return stored_record;
}

std::unique_ptr<rocksdb::Transaction>
BeginTransaction(rocksdb::TransactionDB *db) {
  rocksdb::WriteOptions write_options;
  write_options.sync = true;
  rocksdb::TransactionOptions transaction_options;
  transaction_options.lock_timeout = 1000;
  transaction_options.expiration = 5000;
  std::unique_ptr<rocksdb::Transaction> transaction(
      db->BeginTransaction(write_options, transaction_options));
  if (!transaction) {
    throw std::runtime_error("RocksDB failed to create transaction");
  }
  return transaction;
}

} // namespace

RocksStore::RocksStore(const std::string &db_path) : db_path_(db_path) {
  if (db_path.empty()) {
    throw std::invalid_argument("db_path must not be empty");
  }

  rocksdb::DBOptions db_options;
  db_options.create_if_missing = true;
  db_options.create_missing_column_families = true;
  std::vector<rocksdb::ColumnFamilyDescriptor> column_families = {
      {rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()},
      {kRecordsColumnFamily, rocksdb::ColumnFamilyOptions()},
      {kIndexIdsColumnFamily, rocksdb::ColumnFamilyOptions()},
  };
  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  rocksdb::TransactionDBOptions transaction_db_options;
  transaction_db_options.transaction_lock_timeout = 1000;
  rocksdb::TransactionDB *raw_db = nullptr;
  const rocksdb::Status status =
      rocksdb::TransactionDB::Open(db_options, transaction_db_options, db_path,
                                   column_families, &handles, &raw_db);
  if (!status.ok()) {
    throw std::runtime_error("Failed to open RocksDB at " + db_path + ": " +
                             status.ToString());
  }
  if (handles.size() != 3) {
    for (rocksdb::ColumnFamilyHandle *handle : handles) {
      delete handle;
    }
    delete raw_db;
    throw std::runtime_error(
        "RocksDB returned an unexpected column-family set");
  }

  db_.reset(raw_db);
  default_cf_ = handles[0];
  records_cf_ = handles[1];
  index_ids_cf_ = handles[2];
  try {
    InitializeMetadata();
  } catch (...) {
    delete default_cf_;
    delete records_cf_;
    delete index_ids_cf_;
    default_cf_ = nullptr;
    records_cf_ = nullptr;
    index_ids_cf_ = nullptr;
    db_.reset();
    throw;
  }
}

RocksStore::~RocksStore() {
  delete default_cf_;
  delete records_cf_;
  delete index_ids_cf_;
}

void RocksStore::InitializeMetadata() {
  std::unique_ptr<rocksdb::Transaction> transaction =
      BeginTransaction(db_.get());
  rocksdb::ReadOptions read_options;
  std::string schema_version;
  std::string next_local_id;
  const rocksdb::Status schema_status = transaction->GetForUpdate(
      read_options, default_cf_, AsSlice(kSchemaVersionKey), &schema_version);
  const rocksdb::Status next_id_status = transaction->GetForUpdate(
      read_options, default_cf_, AsSlice(kNextLocalIndexIdKey), &next_local_id);

  if (schema_status.IsNotFound() && next_id_status.IsNotFound()) {
    CheckStatus(transaction->Put(default_cf_, AsSlice(kSchemaVersionKey),
                                 EncodeUint64(kSchemaVersion), true),
                "RocksDB schema-version initialization failed");
    CheckStatus(transaction->Put(default_cf_, AsSlice(kNextLocalIndexIdKey),
                                 EncodeUint64(1), true),
                "RocksDB local-ID initialization failed");
    CheckStatus(transaction->Commit(), "RocksDB metadata commit failed");
    return;
  }
  if (!schema_status.ok() || !next_id_status.ok()) {
    if (schema_status.IsNotFound() || next_id_status.IsNotFound()) {
      throw std::runtime_error("Corrupt RocksDB metadata: schema and local-ID "
                               "counter must both exist");
    }
    CheckStatus(schema_status, "RocksDB schema-version read failed");
    CheckStatus(next_id_status, "RocksDB local-ID read failed");
  }

  if (DecodeUint64(schema_version, "schema version") != kSchemaVersion) {
    throw std::runtime_error("Unsupported RocksDB schema version");
  }
  const uint64_t next_id = DecodeUint64(next_local_id, "local-ID counter");
  if (next_id == 0 ||
      next_id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    throw std::runtime_error("Corrupt RocksDB local-ID counter");
  }
}

RocksStore::PutResult RocksStore::Put(const veclet::v1::VectorRecord &record) {
  PutBatchResult batch_result =
      PutBatch(std::span<const veclet::v1::VectorRecord>(&record, 1));
  return std::move(batch_result.record_results.front());
}

RocksStore::PutBatchResult
RocksStore::PutBatch(std::span<const veclet::v1::VectorRecord> records) {
  constexpr size_t kMaxBatchRecords = 256;
  if (records.empty() || records.size() > kMaxBatchRecords) {
    throw std::invalid_argument("batch must contain 1 to 256 records");
  }

  std::unordered_set<std::string_view> vector_ids;
  vector_ids.reserve(records.size());
  std::vector<size_t> ordered_indexes;
  ordered_indexes.reserve(records.size());
  for (size_t i = 0; i < records.size(); ++i) {
    ValidateRecordEnvelope(records[i]);
    if (!vector_ids.insert(records[i].vector_id()).second) {
      throw std::invalid_argument("batch vector_id values must be unique: " +
                                  records[i].vector_id());
    }
    ordered_indexes.push_back(i);
  }
  std::sort(ordered_indexes.begin(), ordered_indexes.end(),
            [records](size_t lhs, size_t rhs) {
              return records[lhs].vector_id() < records[rhs].vector_id();
            });

  std::unique_ptr<rocksdb::Transaction> transaction =
      BeginTransaction(db_.get());
  rocksdb::ReadOptions read_options;
  PutBatchResult result;
  result.record_results.resize(records.size());
  std::vector<size_t> new_record_indexes;
  new_record_indexes.reserve(records.size());

  for (const size_t index : ordered_indexes) {
    const veclet::v1::VectorRecord &record = records[index];
    std::string existing_value;
    const rocksdb::Status existing_status = transaction->GetForUpdate(
        read_options, records_cf_, record.vector_id(), &existing_value);
    if (existing_status.IsNotFound()) {
      new_record_indexes.push_back(index);
      continue;
    }
    CheckStatus(existing_status, "RocksDB record read failed");

    PutResult &record_result = result.record_results[index];
    record_result.stored_record = ParseStoredRecord(
        existing_value, "for vector_id " + record.vector_id());
    if (!RecordsEqual(record_result.stored_record.record(), record)) {
      throw std::domain_error("insert-only conflict for existing vector_id " +
                              record.vector_id());
    }

    std::string mapped_vector_id;
    const rocksdb::Status mapping_status =
        transaction->Get(read_options, index_ids_cf_,
                         EncodeUint64(static_cast<uint64_t>(
                             record_result.stored_record.local_index_id())),
                         &mapped_vector_id);
    CheckStatus(mapping_status, "RocksDB reverse-mapping read failed");
    if (mapped_vector_id != record.vector_id()) {
      throw std::runtime_error(
          "Corrupt RocksDB reverse mapping for vector_id " +
          record.vector_id());
    }
    ++result.duplicate_records;
  }

  if (new_record_indexes.empty()) {
    return result;
  }

  std::string encoded_next_id;
  CheckStatus(transaction->GetForUpdate(read_options, default_cf_,
                                        AsSlice(kNextLocalIndexIdKey),
                                        &encoded_next_id),
              "RocksDB local-ID read failed");
  const uint64_t next_id = DecodeUint64(encoded_next_id, "local-ID counter");
  const uint64_t max_id =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (next_id == 0 || next_id >= max_id ||
      new_record_indexes.size() > max_id - next_id) {
    throw std::overflow_error("RocksDB local-ID space is exhausted");
  }

  uint64_t local_id = next_id;
  for (const size_t index : new_record_indexes) {
    const veclet::v1::VectorRecord &record = records[index];
    const std::string encoded_local_id = EncodeUint64(local_id);
    std::string conflicting_vector_id;
    const rocksdb::Status mapping_status = transaction->GetForUpdate(
        read_options, index_ids_cf_, encoded_local_id, &conflicting_vector_id);
    if (!mapping_status.IsNotFound()) {
      if (mapping_status.ok()) {
        throw std::runtime_error(
            "Corrupt RocksDB local-ID counter points to an existing mapping");
      }
      CheckStatus(mapping_status, "RocksDB reverse-mapping read failed");
    }

    PutResult &record_result = result.record_results[index];
    record_result.inserted = true;
    record_result.stored_record.set_local_index_id(
        static_cast<int64_t>(local_id));
    *record_result.stored_record.mutable_record() = record;
    std::string serialized;
    if (!record_result.stored_record.SerializeToString(&serialized)) {
      throw std::runtime_error("StoredRecord serialization failed");
    }

    CheckStatus(
        transaction->Put(records_cf_, record.vector_id(), serialized, true),
        "RocksDB record write failed");
    CheckStatus(transaction->Put(index_ids_cf_, encoded_local_id,
                                 record.vector_id(), true),
                "RocksDB reverse-mapping write failed");
    ++result.inserted_records;
    ++local_id;
  }

  CheckStatus(transaction->Put(default_cf_, AsSlice(kNextLocalIndexIdKey),
                               EncodeUint64(local_id), true),
              "RocksDB local-ID advance failed");
  CheckStatus(transaction->Commit(), "RocksDB batch commit failed");
  return result;
}

bool RocksStore::Get(const std::string &vector_id,
                     veclet::storage::v1::StoredRecord *stored_record) const {
  if (vector_id.empty() || vector_id.size() > 256 || !IsValidUtf8(vector_id)) {
    throw std::invalid_argument(
        "vector_id must contain 1 to 256 valid UTF-8 bytes");
  }

  std::string serialized;
  const rocksdb::Status status =
      db_->Get(rocksdb::ReadOptions(), records_cf_, vector_id, &serialized);
  if (status.IsNotFound()) {
    return false;
  }
  CheckStatus(status, "RocksDB record read failed");

  const veclet::storage::v1::StoredRecord parsed =
      ParseStoredRecord(serialized, "for vector_id " + vector_id);
  if (parsed.record().vector_id() != vector_id) {
    throw std::runtime_error("Corrupt RocksDB record key/value mismatch");
  }
  if (stored_record) {
    *stored_record = parsed;
  }
  return true;
}

bool RocksStore::GetByLocalIndexId(
    int64_t local_index_id,
    veclet::storage::v1::StoredRecord *stored_record) const {
  std::string vector_id;
  if (!GetVectorIdByLocalIndexId(local_index_id, &vector_id)) {
    return false;
  }

  veclet::storage::v1::StoredRecord parsed;
  if (!Get(vector_id, &parsed)) {
    throw std::runtime_error(
        "Corrupt RocksDB reverse mapping references a missing record");
  }
  if (parsed.local_index_id() != local_index_id) {
    throw std::runtime_error(
        "Corrupt RocksDB reverse mapping references a different local ID");
  }
  if (stored_record) {
    *stored_record = std::move(parsed);
  }
  return true;
}

bool RocksStore::GetVectorIdByLocalIndexId(int64_t local_index_id,
                                           std::string *vector_id) const {
  if (local_index_id <= 0) {
    throw std::invalid_argument("local_index_id must be positive");
  }

  std::string mapped_vector_id;
  const rocksdb::Status mapping_status = db_->Get(
      rocksdb::ReadOptions(), index_ids_cf_,
      EncodeUint64(static_cast<uint64_t>(local_index_id)), &mapped_vector_id);
  if (mapping_status.IsNotFound()) {
    return false;
  }
  CheckStatus(mapping_status, "RocksDB reverse-mapping read failed");
  if (mapped_vector_id.empty() || mapped_vector_id.size() > 256 ||
      !IsValidUtf8(mapped_vector_id)) {
    throw std::runtime_error(
        "Corrupt RocksDB reverse mapping contains an invalid vector_id");
  }
  if (vector_id) {
    *vector_id = std::move(mapped_vector_id);
  }
  return true;
}

size_t RocksStore::Scan(
    const std::function<bool(const veclet::storage::v1::StoredRecord &)>
        &callback) const {
  std::unique_ptr<rocksdb::Iterator> iterator(
      db_->NewIterator(rocksdb::ReadOptions(), records_cf_));
  size_t count = 0;
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    const veclet::storage::v1::StoredRecord record = ParseStoredRecord(
        std::string_view(iterator->value().data(), iterator->value().size()),
        "during records scan");
    if (iterator->key().ToStringView() != record.record().vector_id()) {
      throw std::runtime_error("Corrupt RocksDB record key/value mismatch");
    }
    ++count;
    if (callback && !callback(record)) {
      break;
    }
  }
  CheckStatus(iterator->status(), "RocksDB records scan failed");
  return count;
}

size_t RocksStore::Count() const { return Scan({}); }

} // namespace veclet::shard
