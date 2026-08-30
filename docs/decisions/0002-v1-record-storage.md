# 0002: V1 record and RocksDB storage

- Status: accepted
- Date: 2026-08-29

## Context

RocksDB is authoritative record state and FAISS is derived. V1 needs one record
shape that can be inserted over gRPC, committed atomically, and replayed to
reconstruct a FAISS index without a string-ID mapping service or source-specific
partition model.

## Decision

Each logical-shard replica owns one RocksDB database. Its `records` column
family uses an eight-byte, big-endian positive `int64` vector ID as the key. The
value is a deterministic protobuf serialization of `veclet.v1.VectorRecord`:

```text
VectorRecord
  vector_id:   positive int64
  version:     positive uint64
  embedding:   repeated float32
  payload_data: UTF-8 string, at most 16 KiB
```

Keeping the ID in both the key and value makes recovery validation explicit.
The record is one value so ID version, embedding, and payload never commit as
separate partial updates. The default column family holds only small system
metadata such as storage-schema version and recovery state; its exact keys land
with the LocalShard state machine.

Float32 is the canonical authoritative embedding because the initial FAISS
indexes consume float32 vectors. Integer and float64 source adapters perform an
explicit, checked conversion to finite float32 before `BatchInsert`. FAISS may
quantize its derived index internally, but quantized bytes do not replace the
authoritative float32 embedding.

V1 has no `partition_column`. Source partitions belong to ingestion checkpoint
and replay state, never the record or query-routing contract. Logical-shard
assignment is derived deterministically from collection identity and vector ID.

## Consequences

String vector IDs require a separately reviewed canonical mapping or a future
API version; V1 avoids collision-prone hashing and mapping state by accepting
positive int64 IDs only. Payload data is stored and returned by no search field
yet, and it is not interpreted as JSON, a filter, or a routing key.

Updates, deletes, tombstones, and source-partition metadata remain deferred.
Snapshots operate at shard-replica granularity, matching placement and recovery
ownership, at the cost of one RocksDB database per hosted shard replica.
