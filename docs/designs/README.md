# Veclet design documents

Design documents describe a proposed or implemented system behavior in enough
detail to review ownership, configuration, failure handling, operations, and
alternatives before code spreads the behavior across components.

Veclet keeps multiple focused designs rather than one ever-growing architecture
document. The [architecture overview](../architecture.md) defines stable public
boundaries; designs explain one bounded area; accepted architecture decisions
under [`docs/decisions/`](../decisions/) record choices that should no longer be
reconsidered casually.

## Statuses

- **Draft:** under discussion and not an implementation or compatibility promise.
- **Accepted:** approved direction; implementation may still be incomplete.
- **Implemented:** shipped behavior with verification evidence.
- **Superseded:** retained for history and linked to its replacement.

## Designs

| Design | Status | Scope |
| --- | --- | --- |
| [0001: Cloud-neutral small-cluster deployment](0001-cloud-neutral-small-cluster-deployment.md) | Draft | Managed cloud, existing-host, and manual deployment modes; six-node placement and networking |
| [0002: DataNode shard placement registry and RPC fencing](0002-data-node-shard-registry.md) | Draft | Monotonic local placement installation, exact removal, and lifetime-safe RPC lookup |
| [0003: Standalone DataNode process](0003-standalone-data-node.md) | Implemented | One-shard process configuration, recovery, gRPC health, and graceful lifecycle |
| [0004: Object-store generation bootstrap and replica recovery](0004-object-store-generation-recovery.md) | Draft | Immutable artifact layout, direct DataNode recovery, fencing, and failure boundaries |
| [0005: Shard placement lifecycle and recovery](0005-shard-placement-lifecycle.md) | Draft | Controller and DataNode phases, failure windows, retry ownership, fencing, and drain |

Each new design should state intended behavior, non-goals, user configuration,
component ownership, state transitions, failure handling, security,
observability, alternatives, and acceptance criteria where applicable.
