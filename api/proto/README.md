# Protobuf contract

`api/proto/veclet/v1` is the single source of truth for Veclet's initial gRPC
boundary. `api/proto/veclet/storage/v1` contains the internal persisted record
format and is never exposed by an RPC. Generated Go and C++ bindings are
intentionally deferred to the language-toolchain change and must never be
edited by hand.

The package remains `veclet.v1` until an explicit version transition. Changes
are additive where possible. Removed field numbers, field names, enum numbers,
and enum names are reserved in the same source file and are never reused.

## V1 scope

- `QueryService.Search` is the client-facing strict distributed search.
- `DataService.SearchShard` searches exactly one fenced logical-shard replica.
- `DataService.BatchInsert` accepts bounded, insert-only, retry-safe mutations.
- `Collection` describes one vector space but has no management service yet.

Controller, routing-watch, build, ingestion, snapshot, and administration RPCs
are deferred until their behavior exists. The protobuf API never exposes
FAISS, RocksDB, Kubernetes, or C++ implementation types.

## Validation boundary

Validation annotations are not added in this change, avoiding a runtime
dependency before generated bindings exist. Every server implementation must
enforce this table before performing work; matching invalid-input tests land
with the implementing RPC.

| Field or message | V1 rule |
| --- | --- |
| `collection_id` | 1–63 lowercase ASCII characters matching `[a-z][a-z0-9-]*`; never normalized |
| `display_name` | At most 128 UTF-8 bytes; non-unique and never used as identity |
| `dimension` | 1–65,536 |
| `metric` | One explicit non-zero `Metric` value |
| `logical_shards` | Omitted resolves to 1; a supplied value must be 1–1,024; responses contain the resolved value |
| Query vector | Exactly `dimension` finite float values; non-zero norm for cosine |
| Stored vector | Exactly `dimension` finite float values; non-zero norm for cosine |
| `k` | 1–1,000 |
| `generation_id` | Positive |
| `shard_id` | Zero-based and less than `logical_shards` |
| `assignment_epoch` | Positive |
| `VectorRecord.vector_id` | 1–256 UTF-8 bytes, exact and unnormalized; numeric IDs use canonical decimal strings |
| `VectorRecord.version` | Positive |
| `VectorRecord.embedding` | Canonical float32; exactly `dimension` finite values; non-zero norm for cosine |
| `VectorRecord.payload_data` | At most 16 KiB of opaque UTF-8; not parsed for filtering, partitioning, or routing |
| `BatchInsertRequest.records` | 1–256 records with unique vector IDs |
| Every RPC message | At most 4 MiB encoded |

Unknown enum values, non-finite floats, missing message fields, size violations,
and range violations are rejected with `INVALID_ARGUMENT` or
`RESOURCE_EXHAUSTED` before mutation or fan-out.

| gRPC status | V1 use |
| --- | --- |
| `INVALID_ARGUMENT` | Malformed identity, boundary violation, unknown metric, invalid vector, or duplicate ID within one batch |
| `NOT_FOUND` | Unknown collection or a shard target not loaded on the contacted DataNode |
| `FAILED_PRECONDITION` | Stale generation/assignment or conflicting insert-only record version/payload |
| `RESOURCE_EXHAUSTED` | Encoded message, batch, queue, or fan-out bound exceeded |
| `UNAVAILABLE` | A strict public search cannot reach every required shard |
| `DEADLINE_EXCEEDED` / `CANCELLED` | The propagated request budget or cancellation ended the operation |
| `INTERNAL` | An unexpected invariant or storage failure, with structured diagnostic context |

## Query and score semantics

Search is strict in v1. A successful response means every logical shard for one
generation and routing epoch contributed. A missing or failed shard returns a
non-OK RPC status, normally `UNAVAILABLE` or `DEADLINE_EXCEEDED`; there is no
partial-success field.

Scores retain FAISS metric semantics:

- L2 is squared Euclidean distance and lower is better.
- Inner product is higher-is-better similarity.
- Cosine is higher-is-better inner product after both stored and query vectors
  are normalized by their owning data/query boundary.

Shard and global results use the same ordering. Equal scores are ordered by
ascending vector-ID UTF-8 bytes so merges are deterministic and locale-free.

## Retry and failure semantics

Clients set one end-to-end deadline and propagate cancellation. Implementations
may retry only within that budget; a nested call never resets it.

`Search` and `SearchShard` are read-only and retry-safe, although a new public
`Search` attempt may observe a newly activated generation. `SearchShard` is
fenced by generation and assignment epoch.

`BatchInsert` validates the whole batch before mutation and acknowledges only
after RocksDB-authoritative state is durable. Replaying the exact same vector
ID, version, embedding, and payload is a no-op counted in `duplicate_records`.
A conflicting record at the same version, a different version for an existing
insert-only record, or a stale generation/assignment rejects the entire batch
with `FAILED_PRECONDITION`; it does not partially apply the request.

The target generation fences mutations against the active immutable base
artifact. `BatchInsert` updates authoritative local record state and its derived
search structure; it never rewrites a published generation artifact.

The durable RocksDB layout wraps this canonical record with a private local
index ID without exposing RocksDB or FAISS labels in the RPC API. It is defined in
[the V1 storage decision](../../docs/decisions/0002-v1-record-storage.md).

PR 003 establishes the first protobuf compatibility baseline because earlier
commits contain no schemas. CI resolves the exact pull-request base or pre-push
commit; it skips comparison only when that commit contains no `.proto` files.
After this baseline merges, Buf's FILE policy applies to every schema change.

See [the V1 contract decision](../../docs/decisions/0001-v1-protobuf-contract.md)
for the tradeoffs fixed by this API.
