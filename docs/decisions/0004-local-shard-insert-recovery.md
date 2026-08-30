# 0004: Local-shard insert and recovery

- Status: accepted
- Date: 2026-08-30

## Context

RocksDB is the authoritative local record state while FAISS is a derived search
structure. A process can stop or FAISS can fail after a durable record commit,
so an insert cannot be treated as one atomic mutation across both systems.

## Decision

`LocalShard::Put` is insert-only. It first validates the complete record, then
uses one RocksDB transaction to allocate a positive local index ID, write the
external-ID record, write the reverse local-ID mapping, and advance the local-ID
counter. Only after that durable commit does it add the vector to FAISS.

Exact retries return the original local ID and do not create another record or
FAISS label. Reusing an external ID with different record content is rejected.
Deletes, updates, and tombstones remain unsupported in v1.

The write state machine is:

| State | Durable authority | Externally visible behavior | Next action |
| --- | --- | --- | --- |
| validated | no new record | no acknowledgement | begin RocksDB transaction |
| RocksDB committed | record and both mappings | no acknowledgement yet | add the vector to FAISS |
| indexed | same durable record | acknowledge success | exact retries are no-ops |
| derived index unhealthy | durable record may exist | fail search and insert closed | restart and rebuild from RocksDB |

The crash and failure model is:

| Window | Result | Recovery |
| --- | --- | --- |
| before RocksDB commit | no partial record or counter advance | retry the insert |
| after RocksDB commit, before FAISS add | authoritative record exists without a searchable label | restart rebuilds FAISS from every RocksDB record |
| FAISS add throws | mutation outcome may be ambiguous | mark the derived index unhealthy and restart; do not serve potentially partial search results |
| acknowledgement is lost | record and FAISS label exist | exact retry resolves the existing record and is a no-op |

Recovery starts with an empty index, validates every record and reverse mapping,
trains the index when required, and rebuilds FAISS. A corrupt record, missing
mapping, duplicate local ID, training failure, or add failure prevents the shard
from starting. The owning process is responsible for not publishing the shard
as READY until construction and recovery finish.

`LocalShard` serializes FAISS mutation with an exclusive lock and permits
concurrent searches with a shared lock. RocksDB calls happen outside that lock.
RocksDB transactions have bounded lock and expiration times and are not retried
inside the storage layer; callers retry only the idempotent insert operation.

## Consequences

A committed insert can return an error when its derived FAISS update fails. The
record remains authoritative and becomes searchable after restart recovery.
Failing closed sacrifices temporary availability but avoids silent partial
search results or guessing whether a FAISS mutation partially completed.

Search resolves each returned local label through the `index_ids` column family
and fails on a missing mapping. Equal scores are ordered by exact external
vector-ID UTF-8 bytes, independent of replica-local FAISS labels.
