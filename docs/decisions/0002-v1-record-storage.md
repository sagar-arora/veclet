# 0002: V1 record and RocksDB storage

- Status: accepted
- Date: 2026-08-29

## Context

User identifiers are commonly UUIDs, document keys, and SKUs. FAISS accepts
only signed 64-bit labels, while RocksDB must remain authoritative enough to
reconstruct both the index and the user-ID mapping without collision risk.

## Decision

The V1 API uses one exact, unnormalized UTF-8 string ID of 1–256 bytes. Numeric
IDs use their canonical decimal string, so `"1"` and `"01"` are intentionally
different unless a client normalizes them before insertion.

Each logical-shard replica owns one RocksDB database with this layout:

| Column family | Key | Value |
| --- | --- | --- |
| `records` | External vector-ID UTF-8 bytes | Deterministic `veclet.storage.v1.StoredRecord` protobuf |
| `index_ids` | Eight-byte big-endian positive local index ID | External vector-ID UTF-8 bytes |
| default | Small fixed system keys | Schema version, next local index ID, and recovery state |

`StoredRecord` contains the public `VectorRecord` plus a positive, shard-local
`int64 local_index_id` used only as the FAISS label. Allocating a new local ID,
writing both mappings, and advancing the counter occur in one RocksDB
`WriteBatch`. Replicas may allocate different local labels because queries
resolve labels locally before returning the same external string ID.

Float32 is the canonical authoritative embedding because the initial FAISS
indexes consume float32 vectors. Integer and float64 source adapters perform an
explicit, checked conversion to finite float32 before `BatchInsert`. FAISS may
quantize its derived index internally, but quantized bytes do not replace the
authoritative float32 embedding.

V1 has no `partition_column`. Source partitions belong to ingestion checkpoint
and replay state, never the record or query-routing contract. Logical-shard
assignment is derived deterministically from collection identity and external
vector-ID bytes.

## Consequences

The reverse `index_ids` mapping is unavoidable because FAISS cannot return
string labels. It adds one small RocksDB entry per record but avoids hash
collisions and a global ID-allocation service. Payload data is stored but is not
yet returned, parsed as JSON, filtered, or used as a routing key.

Updates, deletes, tombstones, and source-partition metadata remain deferred.
Snapshots operate at shard-replica granularity, matching placement and recovery
ownership, at the cost of one RocksDB database per hosted shard replica.
