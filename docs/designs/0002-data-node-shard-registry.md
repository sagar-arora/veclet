# 0002: DataNode shard placement registry and RPC fencing

- Status: draft
- Date: 2026-08-30

## Intent

A DataNode needs one local authority for deciding whether an internal shard RPC
names its current physical placement. The registry maps collection and
logical-shard identity to one READY `LocalShard`, immutable generation ID, and
controller-issued placement epoch.

The DataNode replica manager is the only future writer of this registry. It
acts on controller placement intent, but calls `Install` only after any remote
artifacts are downloaded, verified, and opened as a READY `LocalShard`.
`SearchShard` and `BatchInsert` look up and validate placements but cannot
create, advance, or replace them. The current in-process API is a bootstrap and
test boundary until that authenticated control path exists.

## Terms and concrete lifecycle

This design uses a few names that are easy to confuse:

- A **logical shard** is a numbered slice of one collection, for example
  `products` shard `3`. It is not a machine.
- A **LocalShard** is the C++ object on one DataNode that owns that slice's
  local RocksDB records and derived FAISS index.
- A **shard placement** says that a particular DataNode is currently allowed to
  serve one logical shard. It contains the collection, shard, generation, and
  placement epoch.
- The **controller** is the future cluster-management component that decides
  placement. It watches the desired cluster state and tells DataNodes which
  placements to prepare. It does not receive or proxy normal searches or
  generation artifacts.
- The DataNode **replica manager** performs slow, cancellable preparation such
  as fetching an object-store manifest, restoring RocksDB, and loading or
  rebuilding FAISS. Only this owner installs the resulting READY `LocalShard`
  in the local registry.

For a single-node developer deployment, setup code initially installs every
placement locally. In a future cluster, the controller will send placement
intent over an authenticated, reconnectable control stream. That stream and
the replica manager are not implemented by this design: `Install` and `Remove`
are currently in-process bootstrap/test methods, not controller commands or
public client RPCs. Object-store preparation is specified separately in
[Design 0004](0004-object-store-generation-recovery.md).

### When a placement is installed

A local registry installation happens after the controller has made a
placement decision and the DataNode replica manager has prepared a READY local
replica. Typical events are:

1. A user creates `products`; the controller divides it into logical shards
   and assigns each replica to eligible DataNodes.
2. A new DataNode joins, so the controller moves a replica to improve balance.
3. The index is rebuilt as a new immutable generation and the replacement
   artifact is ready on a node.
4. A failed or draining node is replaced, after the required replica is ready
   elsewhere.

For example, if the controller decides that DataNode B should now serve
`products` shard `3`, generation `7`, it asks B to prepare a placement with a
new epoch, say `21`. B obtains and verifies the shard's artifacts, opens the
`LocalShard`, and only then installs that pointer in its local registry. After B
reports READY and the controller publishes the route, B can accept
`SearchShard` requests that name that exact placement.

### When a placement is removed

A local registry removal happens when the replica manager processes controller
intent that no longer wants *that exact replica* to serve traffic: after a
replacement is ready, while draining a node, when deleting a collection, or
after a failed replica has been declared unavailable. The removal names the
full prior placement: collection, shard, generation, and epoch.

The exact-match rule is deliberate. Commands can be delayed or duplicated. If
an old "remove epoch 20" command reaches a node after that node has installed
epoch 21, it must do nothing; otherwise a stale message could accidentally
remove the new replica.

```mermaid
sequenceDiagram
    participant CTL as Controller (future)
    participant A as DataNode A
    participant B as DataNode B replica manager
    participant R as DataNode B registry
    participant O as Object store
    participant Q as QueryNode

    Note over CTL: Move products / shard 3 from A to B
    CTL->>B: Prepare placement: generation 7, epoch 21, manifest URI
    B->>O: Fetch and verify shard artifacts
    O-->>B: Immutable generation bytes
    Note over B: Restore RocksDB; load or rebuild FAISS
    B->>R: Install already-READY LocalShard
    B-->>CTL: Placement READY
    CTL->>Q: Publish route to B, epoch 21
    Q->>B: SearchShard(products, 3, generation 7, epoch 21)
    B-->>Q: Search result
    CTL->>A: Stop serving placement: generation 7, epoch 20
    Note over A: Removes only if A still has exactly epoch 20
```

The controller must publish a route only after the target replica is READY and
must preserve the required replication factor before draining the old replica.
Those distributed placement and replication protocols are intentionally outside
this local-registry slice.

## Invariants

- Collection identity matches `[a-z][a-z0-9-]{0,62}`.
- Generation ID and placement epoch are positive.
- One collection and logical shard key has at most one active local placement.
- Replacement requires a strictly newer placement epoch.
- Replacement cannot move generation backward.
- Repeating the exact placement with the same `LocalShard` is idempotent.
- Reusing an epoch for different placement state or a different `LocalShard` is a
  conflict.
- Remove deletes only an exact collection, shard, generation, and epoch.
- Registry locks are never held during RocksDB or FAISS work.

## State transitions

| Current state | Command | Precondition | Result |
| --- | --- | --- | --- |
| absent | install | valid placement and non-null READY shard | active placement installed |
| active `(G, E)` | install `(G, E)` | exact same placement and shard object | no-op |
| active `(G, E)` | install `(G2, E2)` | `E2 > E` and `G2 >= G` | old handle revoked; replacement active |
| active `(G, E)` | install | stale epoch, reused epoch, or lower generation | reject; current placement unchanged |
| active `(G, E)` | remove `(G, E)` | exact placement | handle revoked and key removed |
| active `(G, E)` | remove another placement | none | no-op; current placement unchanged |
| absent | remove | valid placement | no-op |
| any | registry destruction | none | all outstanding handles revoked |

Lookup returns a shared placement handle, so the `LocalShard` remains alive
after the registry lock is released. RPC code checks `active` before starting
work and again before acknowledging. A replacement revokes the old handle before
publishing its successor in the local map.

## `SearchShard` behavior

`SearchShard` is the internal QueryNode-to-DataNode read path. Its request names
one exact `ShardPlacement`; it cannot install a placement by presenting a larger
epoch. The handler proceeds in this order:

1. Reject cancellation, an encoded request over 4 MiB, a missing placement,
   non-positive generation or placement epoch, or `k` outside 1–1,000.
2. Look up `(collection_id, shard_id)` in the local registry.
3. Return `NOT_FOUND` when this DataNode has no placement for that key, or
   `FAILED_PRECONDITION` when its generation or placement epoch differs.
4. Validate the query dimension, finite values, and non-zero cosine norm.
5. Search the `LocalShard` without holding the registry mutex.
6. Recheck cancellation and the placement handle before constructing a
   successful response. If replacement won the race, discard the result and
   return `FAILED_PRECONDITION`.

The final active check is the read's placement linearization point. If it occurs
before replacement, the read belongs to the old placement; if revocation occurs
first, the old result is not acknowledged. `LocalShard::Search` and FAISS are
synchronous and cannot currently be interrupted halfway through a call, so the
handler checks cancellation immediately before and after that bounded local
work. The caller still owns and propagates the end-to-end gRPC deadline.

Unexpected FAISS or RocksDB failures return `INTERNAL`. Invalid query data
returns `INVALID_ARGUMENT`. A successful response echoes the registry's exact
placement rather than trusting a separately constructed response value.

## `BatchInsert` behavior

`BatchInsert` is the internal insert-only write path to one DataNode replica.
The request names one exact `ShardPlacement`; a caller cannot promote itself by
presenting a larger placement epoch. The handler proceeds in this order:

1. Reject cancellation, an encoded request over 4 MiB, a missing placement,
   non-positive generation or placement epoch, an empty batch, or more than 256
   records.
2. Look up `(collection_id, shard_id)` and require the exact active placement.
3. Copy the bounded protobuf records into a contiguous local batch, then recheck
   cancellation and placement activity immediately before mutation.
4. Ask `LocalShard` to validate every record and apply the batch. Any invalid or
   conflicting record prevents the RocksDB transaction from committing.
5. After RocksDB and the derived FAISS update complete, recheck cancellation and
   placement activity before acknowledging success.
6. Verify that inserted plus duplicate counts equal the request size, then echo
   the registry's placement and counts.

An exact replay returns `OK` with every existing identical record counted in
`duplicate_records`. A different version, embedding, or payload for an existing
vector ID returns `FAILED_PRECONDITION`. Invalid record fields or duplicate IDs
inside the request return `INVALID_ARGUMENT`; the encoded-request and upper
batch bounds return `RESOURCE_EXHAUSTED`. Unexpected RocksDB or FAISS failures
return `INTERNAL` with placement context.

The mutation outcome table is:

| Event | Durable local result | RPC result |
| --- | --- | --- |
| cancelled or revoked before `LocalShard::PutBatch` | no new records | `CANCELLED` or `FAILED_PRECONDITION` |
| record validation or insert-only conflict | no new records from the batch | `INVALID_ARGUMENT` or `FAILED_PRECONDITION` |
| RocksDB transaction setup or staging fails before commit begins | no partial batch write | `INTERNAL` |
| RocksDB commit reports an ambiguous failure | batch may already be durable | `INTERNAL`; exact retry is safe |
| RocksDB and FAISS complete while placement stays active | complete local batch | `OK` with inserted and duplicate counts |
| RocksDB commits but FAISS update fails | complete authoritative batch; derived index fails closed | `INTERNAL`; restart rebuilds FAISS |
| cancellation or revocation wins after local mutation starts | batch may already be durable | no success acknowledgement; exact retry is safe |

The last row is deliberately not described as rollback. RocksDB is authoritative
and a placement flag cannot cancel a transaction that is already committing.
Normal controller-driven replacement must therefore drain bounded in-flight
writes before removing a placement. Replicating or catching up those writes on a
replacement node belongs to the separate replication protocol.

## Failure boundary

Revocation prevents new local RPCs from acquiring an old registry entry and
lets completed searches or writes be rejected before acknowledgement. It does
not by itself stop a mutation already executing inside RocksDB, revoke a node
isolated from the controller, or provide replicated write fencing. Normal
placement changes must drain bounded in-flight work. Forced failover, leases,
write replication, and catch-up require the separate replication design
described as out of scope by Design 0001.

## Acceptance criteria

- Concurrent lookup never retains the registry mutex during shard work.
- A delayed install cannot replace a higher epoch.
- A delayed remove cannot remove a higher epoch.
- Repeated exact installation is idempotent.
- Conflicting epoch reuse and generation rollback fail explicitly.
- Replacement and destruction make previously returned handles inactive.
- `SearchShard` validates documented request bounds and returns deterministic
  gRPC status categories.
- A placement replacement during search prevents the old result from being
  acknowledged.
- `BatchInsert` validates the complete bounded batch before durable mutation,
  reports exact retries, and maps conflicts without partial writes.
- Cancellation or placement replacement prevents a successful write
  acknowledgement, including when the local batch has already committed.
