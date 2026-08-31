# 0001: Cloud-neutral small-cluster deployment

- Status: draft
- Date: 2026-08-30

## Intent

Veclet treats deployment simplicity as a product constraint. One developer
should be able to take one or three ordinary Linux machines from
infrastructure-ready to the first successful insert and search within minutes.
The same runtime and primary configuration model must extend to six-node and
larger clusters without introducing a different data plane.

This design covers three deployment modes:

1. Veclet-managed cloud infrastructure.
2. Existing cloud instances discovered through provider metadata or tags.
3. Manually supplied Linux or bare-metal hosts.

AWS, GCP, Azure, Oracle Cloud, Kubernetes, and bare-metal integrations remain
thin provisioning or orchestration adapters. They do not own logical-shard
placement, routing, replication state, or database transitions.

## Non-goals

This design does not approve a replication protocol, automatic write failover,
cross-region consistency, controller metadata high availability, Kubernetes
operators, or autoscaling. Those behaviors require focused designs and failure
models before implementation. This document distinguishes desired replica
placement from replicated durability and does not claim that the latter exists.

## Common runtime topology

A small cluster may colocate roles to reduce cost while preserving component
boundaries. A six-machine profile can run:

| Machine | Processes |
| --- | --- |
| Node 1 | Controller, QueryNode, DataNode |
| Node 2 | QueryNode, DataNode |
| Nodes 3–6 | DataNode |

All six machines contribute storage. Two stateless QueryNodes provide multiple
client endpoints. The single durable controller is the simplest initial
profile: a temporary controller outage stops collection and placement changes,
but QueryNodes continue using their last-known-good routing snapshot. This is
not a controller high-availability claim.

At larger scale, roles may move to dedicated machines without changing the
protobuf contracts or C++ data plane.

## Configuration ownership

Users specify three different kinds of intent at different times:

- **Infrastructure intent:** machine count and shape, disks, region, and failure
  domains.
- **Cluster intent:** storage paths, placement defaults, service counts,
  security, and observability.
- **Collection intent:** dimension, metric, logical shards, replication factor,
  and index configuration.

Users do not manually assign collection shards to individual machines. The
controller derives and maintains physical placements from collection intent and
live node capacity.

## Deployment mode A: managed cloud infrastructure

When Veclet provisions the machines, the user does not provide their addresses.
A provider adapter creates instances and disks, discovers private addresses and
failure domains, configures security rules, bootstraps the controller, and gives
each node a short-lived join credential.

An AWS-oriented infrastructure request may look like:

```yaml
provider: aws
region: us-west-2
availability_zones:
  - us-west-2a
  - us-west-2b
  - us-west-2c

instances:
  count: 6
  instance_type: m4.xlarge
  data_volume_gib: 500
```

Equivalent provider adapters translate the same intent to their native compute,
network, disk, identity, and failure-domain resources. Provider fields never
enter Veclet's protobuf or persisted shard formats.

## Deployment mode B: existing cloud instances

For existing cloud machines, discovery by tags or labels is preferred over a
static address inventory:

```yaml
provider: aws

existing_instances:
  discover_by_tags:
    VecletCluster: production
```

The deployment adapter resolves matching instances to private addresses,
verifies they belong to the expected network and account, and installs the same
provider-neutral node configuration used by managed mode. Discovery must fail
when the result is empty, ambiguous, duplicated, publicly addressed when private
addressing is required, or different from the requested machine count.

## Deployment mode C: manual or bare-metal hosts

Manual inventory is the fallback when no trusted discovery mechanism exists.
Prefer stable private DNS names over addresses that may change:

```yaml
existing_instances:
  hosts:
    - veclet-1.internal
    - veclet-2.internal
    - veclet-3.internal
    - veclet-4.internal
    - veclet-5.internal
    - veclet-6.internal
```

The host list is deployment input, not the runtime membership database. After
bootstrap, nodes register with the controller and membership is maintained by
assignment state and heartbeats.

## Node bootstrap and discovery

Every node receives a minimal generated configuration:

```yaml
controller:
  endpoint: veclet-controller.internal:7400

node:
  node_id: auto
  advertise_address: auto
  join_token_file: /etc/veclet/join-token
  data_directory: /var/lib/veclet
```

`auto` means the deployment adapter supplies a stable node identity and private
advertised address. The core runtime validates those values but does not depend
on a particular provider's instance-metadata service. A manually installed node
may set the node ID and private address explicitly.

The join credential authorizes initial registration only. Long-lived process
identity uses rotated workload credentials. Registration reports the node ID,
private data-plane address, failure domain, available resources, software
version, and supported capabilities.

## Collection example

Collections are created after the cluster is running:

```yaml
collection_id: products
dimension: 384
metric: cosine
logical_shards: 12
replication_factor: 2

index:
  type: hnsw
  m: 32
  ef_search: 64
```

Logical-shard count is independent of machine count. In this example, twelve
logical shards with replication factor two produce twenty-four physical
replicas, or approximately four replicas per DataNode in a balanced six-node
cluster.

## Placement

The controller owns desired placement. DataNodes report capacity and current
replica state, but they do not choose their own shards or increment assignment
epochs.

Placement follows these rules in order:

1. Never place two replicas of one logical shard on the same DataNode.
2. Spread replicas across failure domains when enough domains exist.
3. Reject nodes without required disk, memory, version, or index capability.
4. Balance estimated bytes and resource demand, not only replica count.
5. Prefer an existing safe placement to avoid unnecessary data movement.
6. Use stable node IDs for deterministic tie-breaking.

With six nodes across three availability zones, an illustrative balanced
placement is:

| Logical shards | First replica | Second replica |
| --- | --- | --- |
| 0 and 6 | Node 1, zone A | Node 3, zone B |
| 1 and 7 | Node 2, zone A | Node 5, zone C |
| 2 and 8 | Node 3, zone B | Node 6, zone C |
| 3 and 9 | Node 4, zone B | Node 1, zone A |
| 4 and 10 | Node 5, zone C | Node 4, zone B |
| 5 and 11 | Node 6, zone C | Node 2, zone A |

Real placement uses measured capacity and existing assignments; the table is
not a hard-coded ring.

## Assignment lifecycle

Each physical replica has a controller-issued assignment epoch. A normal
placement transition is:

| State | Routable | Meaning |
| --- | --- | --- |
| `ASSIGNED` | no | Controller selected a node and issued a newer assignment epoch |
| `RECOVERING` | no | DataNode is creating or copying authoritative state and preparing FAISS |
| `READY` | yes | Required state and artifacts were verified for the exact assignment |
| `DRAINING` | no for new work | Existing bounded work is finishing while replacement safety is established |
| `REVOKED` | no | The assignment can no longer accept work |

Only `READY` replicas enter routing. The controller publishes a replacement
only after it has the required verified replicas, and it does not remove a
draining replica until replication safety is restored.

If a node stops heartbeating, the controller first marks it suspect, then
unavailable after the configured failure policy. Searches may use remaining
READY replicas. A replacement receives a newer assignment epoch and remains
outside routing until recovery completes.

## Replication boundary

Creating two placements does not by itself replicate records. RocksDB provides
authoritative local durability but not network replication. FAISS remains a
derived structure and is never a replication source of truth.

A deliberately simple candidate for the first replication design is
synchronous fan-out of insert-only, idempotent records to every required
replica, with success acknowledged only after all required replicas commit.
That design favors correctness and simple recovery over write availability: an
affected shard stops accepting successful writes while a required replica is
unavailable. Replica creation still needs a reviewed snapshot and catch-up
barrier before becoming `READY`.

Concurrent conflicting inserts, quorum acknowledgement, missed-write repair,
forced failover during a network partition, and replica catch-up are not solved
by assignment epochs. Veclet must not claim replicated durability until a
separate accepted replication design defines those cases and tests them.

## Networking

Heartbeats do not require an all-to-all heartbeat mesh. Each node maintains a
controller connection, while other internal paths reflect actual data flow:

| Source | Destination | Purpose |
| --- | --- | --- |
| Client or load balancer | QueryNodes | Public search API |
| DataNodes and QueryNodes | Controller | Registration, heartbeat, assignment, and routing streams |
| QueryNodes | DataNodes | Shard searches |
| Insert coordinator | DataNodes | Shard inserts and retries |
| DataNodes | DataNodes | Approved snapshot or replica-bootstrap transfer |
| Metrics collector | All processes | Optional metrics scraping or collection |

Controller and DataNode endpoints remain private. In a cloud VPC, the simplest
initial rule is a cluster security group that permits only documented internal
ports from itself, while the load balancer reaches only QueryNode client ports.
The implementation should use one long-lived bidirectional control stream for
heartbeats and assignments rather than a separate public heartbeat port.

All internal connections authenticate workload identity and encrypt traffic.
Raw cloud credentials and join tokens never appear in protobuf messages, logs,
images, or committed configuration. Administrative access should use the
provider's managed session facility where available rather than public SSH.

## Capacity and preflight

Veclet publishes profiles for representative 10-million, 20-million, and
approximately 100-million-vector datasets. Every profile states dimension,
index type and parameters, replication factor, CPU, RAM, disk, ingest and query
rates, payload size, and recovery headroom. A record count or machine type alone
is never a capacity guarantee.

Before installation or assignment, preflight checks validate at least:

- private address reachability and required ports;
- stable node identity and distinct failure domains;
- disk path, capacity, permissions, and minimum free-space reserve;
- estimated RocksDB, FAISS, and recovery working-set requirements;
- software and persisted-format compatibility;
- workload identity and certificate validity; and
- enough eligible nodes to satisfy the requested replication factor.

An unsafe cluster or collection configuration fails before serving traffic
rather than silently weakening replication or durability.

## Observability

Every process emits structured logs and bounded-cardinality metrics for
registration, heartbeat state, assignment transitions, recovery progress,
readiness, storage capacity, and RPC outcomes. Deployment adapters may connect
those signals to Prometheus, Grafana, or an OpenTelemetry-compatible collector
without making a particular vendor part of core correctness.

## Acceptance criteria

This design is ready to move from draft to accepted when review agrees on:

- the three deployment modes and their configuration ownership;
- the small-cluster role topology and placement invariants;
- the node bootstrap, identity, and private-network boundaries;
- measured single-node and three-node time-to-first-insert/search targets;
- capacity-profile inputs and preflight failure behavior; and
- explicit separation between replica placement and the future replication
  protocol.

Implementation is complete only when automated tests provision representative
environments, verify the first insert and search, exercise node loss and
recovery, confirm that only READY replicas are routed, and prove that provider
adapters produce behaviorally equivalent Veclet configuration.
