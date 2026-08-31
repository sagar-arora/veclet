# 0002: DataNode shard registry and RPC fencing

- Status: draft
- Date: 2026-08-30

## Intent

A DataNode needs one local authority for deciding whether an internal shard RPC
targets its current physical assignment. The registry maps collection and
logical-shard identity to one READY `LocalShard`, immutable generation ID, and
controller-issued assignment epoch.

The controller assignment channel is the only future writer of this registry.
`SearchShard` and `BatchInsert` may look up and validate assignments but cannot
create, advance, or replace them. The current in-process API is a bootstrap and
test boundary until that authenticated channel exists.

## Invariants

- Collection identity matches `[a-z][a-z0-9-]{0,62}`.
- Generation ID and assignment epoch are positive.
- One collection and logical shard key has at most one active local assignment.
- Replacement requires a strictly newer assignment epoch.
- Replacement cannot move generation backward.
- Repeating the exact target with the same `LocalShard` is idempotent.
- Reusing an epoch for different target state or a different `LocalShard` is a
  conflict.
- Unregister removes only an exact collection, shard, generation, and epoch.
- Registry locks are never held during RocksDB or FAISS work.

## State transitions

| Current state | Command | Precondition | Result |
| --- | --- | --- | --- |
| absent | register | valid target and non-null READY shard | active assignment installed |
| active `(G, E)` | register `(G, E)` | exact same target and shard object | no-op |
| active `(G, E)` | register `(G2, E2)` | `E2 > E` and `G2 >= G` | old handle revoked; replacement active |
| active `(G, E)` | register | stale epoch, reused epoch, or lower generation | reject; current assignment unchanged |
| active `(G, E)` | unregister `(G, E)` | exact target | handle revoked and key removed |
| active `(G, E)` | unregister another target | none | no-op; current assignment unchanged |
| absent | unregister | valid target | no-op |
| any | registry destruction | none | all outstanding handles revoked |

Lookup returns a shared assignment handle, so the `LocalShard` remains alive
after the registry lock is released. RPC code checks `active` before starting
work and again before acknowledging. A replacement revokes the old handle before
publishing its successor in the local map.

## Failure boundary

Revocation prevents new local RPCs from acquiring an old registry entry and lets
completed searches be discarded before acknowledgement. It does not by itself
stop a mutation already executing inside RocksDB, revoke a node isolated from
the controller, or provide replicated write fencing. Normal placement changes
must drain bounded in-flight work. Forced failover, leases, write replication,
and catch-up require the separate replication design described as out of scope
by Design 0001.

## Acceptance criteria

- Concurrent lookup never retains the registry mutex during shard work.
- A delayed registration cannot replace a higher epoch.
- A delayed unregister cannot remove a higher epoch.
- Repeated exact registration is idempotent.
- Conflicting epoch reuse and generation rollback fail explicitly.
- Replacement and destruction make previously returned handles inactive.
