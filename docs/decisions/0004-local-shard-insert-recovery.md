# 0004: Local-shard insert and recovery

- Status: accepted
- Date: 2026-08-30

## Context

RocksDB is the authoritative local record state while FAISS is a derived search
structure. A process can stop or FAISS can fail after a durable record commit,
so an insert cannot be treated as one atomic mutation across both systems.

## Decision

`LocalShard::Put` and `LocalShard::PutBatch` are insert-only. A batch contains
1–256 records with unique external IDs. The complete batch is validated before
storage changes. One RocksDB transaction allocates positive local index IDs,
writes every new external-ID record and reverse local-ID mapping, and advances
the local-ID counter once. Only after that durable commit does one FAISS `Add`
call install every label that is not already present in the derived index.

Exact retries return the original local IDs and do not create another record or
FAISS label. A retry also finishes a missing derived-index update if another
attempt committed RocksDB but did not install its FAISS label. Reusing any
external ID with different record content rejects the entire batch. Deletes,
updates, and tombstones remain unsupported in v1.

The write state machine is:

| State | Durable authority | Externally visible behavior | Next action |
| --- | --- | --- | --- |
| validated | no new records | no acknowledgement | begin RocksDB transaction |
| RocksDB committed | all new records and both mappings | no acknowledgement yet | add every missing label to FAISS in one call |
| indexed | same durable records | acknowledge batch counts | exact retries verify that every label is present |
| derived index unhealthy | all committed records remain durable | fail search and insert closed | restart and rebuild from RocksDB |

The crash and failure model is:

| Window | Result | Recovery |
| --- | --- | --- |
| validation or conflict rejection | no batch record or counter change | correct the batch or retry exact records |
| before RocksDB commit | no partial batch record or counter advance | retry the complete batch |
| after RocksDB commit, before FAISS add | every authoritative batch record exists without some searchable labels | an in-process exact retry finishes missing labels; restart rebuilds FAISS from every RocksDB record |
| FAISS add throws | mutation outcome may be ambiguous | mark the derived index unhealthy and restart; do not serve potentially partial search results |
| acknowledgement is lost | records and FAISS labels exist | exact retry resolves the existing records without duplicate labels |

Recovery starts with an empty, already-trained index, validates every record and
reverse mapping, and rebuilds FAISS in batches bounded to at most 1,024 vectors
and approximately 1,048,576 float values. Index training and loading immutable
trained artifacts belong to the generation lifecycle rather than local record
recovery. A corrupt record, missing mapping, duplicate local ID, or add failure
prevents the shard from starting. The owning process is responsible for not
publishing the shard as READY until construction and recovery finish.

`LocalShard` serializes FAISS mutation with an exclusive lock and permits
concurrent searches with a shared lock. RocksDB calls happen outside that lock.
Record keys are locked in external-ID byte order so overlapping batches use a
stable lock order. RocksDB transactions have bounded lock and expiration times
and are not retried inside the storage layer; callers retry only the idempotent
insert operation.

## Consequences

A committed batch can return an error when its derived FAISS update fails. All
records remain authoritative and become searchable after restart recovery.
Failing closed sacrifices temporary availability but avoids silent partial
search results or guessing whether a FAISS mutation partially completed.

Search resolves each returned local label through the `index_ids` column family
and fails on a missing mapping. Search adaptively requests enough candidates to
resolve an equal-score top-K boundary by exact external vector-ID UTF-8 bytes,
independent of replica-local FAISS labels. Candidate expansion is bounded at
10,000 entries; a larger unresolved tie fails explicitly instead of returning a
nondeterministic subset.
