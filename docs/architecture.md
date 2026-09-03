# Architecture overview

This document describes Veclet's public architectural boundaries. It is not an
implementation schedule.

## Components

### Controller

The Go controller owns collection metadata, generation metadata, node health,
logical-shard placement, and monotonically versioned routing snapshots. It
does not store vectors or proxy steady-state searches.

### QueryNode

QueryNodes are stateless C++ query coordinators. A QueryNode resolves a
collection's current routing snapshot, sends each logical shard request to one
READY replica, and deterministically merges shard-local results into a global
top-K response. Any QueryNode can serve any collection for which it has a valid
routing snapshot, so scaling QueryNodes does not move data.

### DataNode

DataNodes are C++ storage and search processes. RocksDB holds authoritative
local records. FAISS indexes are derived search structures that can be rebuilt
from authoritative state or loaded from verified, immutable generation
artifacts.

### Optional ingestion

Ingestion integrations route mutations by logical shard. An upstream log such
as Kafka may provide durable delivery, but it does not replace Veclet's local
durability or become a query-routing concept.

## Resource model

- A **collection** defines a vector namespace and its index configuration.
- A **logical shard** partitions a collection independently of physical nodes.
- A **replica** is one physical copy of a logical shard on a DataNode.
- A **generation** is an immutable set of verified index artifacts activated
  atomically for a collection or shard.
- A **routing snapshot** maps logical shards to READY replicas and carries a
  monotonically increasing version or epoch.

Multiple teams create collections through the same control plane; they do not
deploy a controller per collection. Collections may use different index
configurations and generations. Query capacity is a shared stateless pool by
default, with optional workload isolation through separate QueryNode pools and
routing policy when operationally required.

## Query path

1. A client sends a collection-scoped search to a QueryNode.
2. The QueryNode uses its last-known-good, versioned routing snapshot.
3. It selects one READY replica for every logical shard in the collection.
4. DataNodes run shard-local FAISS searches.
5. The QueryNode merges results using metric-aware ordering and deterministic
   tie-breaking.
6. Missing shards produce an explicit error or documented partial response;
   they never become silent success.

The controller is not on this steady-state path. QueryNodes retain a
last-known-good snapshot during temporary controller unavailability.

## Placement and recovery

Placement is deterministic for identical inputs and keeps logical-shard
identity separate from DataNode identity. Only verified READY replicas enter
routing. Drain and scale-in first establish the required replication factor,
then remove the old placement. Placement epochs or fencing tokens prevent
delayed workers from publishing stale state.

RocksDB/FAISS mutations, generation publication, routing publication, and
acknowledgements require explicit crash-window and recovery definitions before
implementation. Generation artifacts are immutable, checksummed, verified
before activation, and switched atomically.

## Deployment model

Standalone mode runs without Kubernetes, Istio, Kafka, Airflow, or a separate
object-store service. Clustered mode adds orchestration, identity, networking,
and durable artifact storage without changing the protobuf contracts or C++
data plane.

Deployment simplicity is an architectural constraint, not packaging work left
until after the data plane. Veclet maintains opinionated single-node and
three-node profiles. Once Linux compute, networking, and disks are available, a
developer should be able to generate the configuration, start the processes,
pass readiness checks, and perform the first insert and search within minutes.
The workflow and configuration model remain provider-neutral across AWS, GCP,
Azure, Oracle Cloud, and bare metal; provider-specific modules are thin,
optional infrastructure adapters.

The managed-cloud, existing-instance, and manual-host workflows are specified
in [Design 0001: Cloud-neutral small-cluster deployment](designs/0001-cloud-neutral-small-cluster-deployment.md).

Capacity profiles cover representative datasets in the 10-million,
20-million, and approximately 100-million-vector ranges. A vector count alone
is never a capacity guarantee: every profile and benchmark states dimension,
metric, index type and parameters, replication factor, ingest and query rates,
RAM, CPU, disk, and recovery headroom. Preflight validation should reject an
obviously unsafe configuration before processes begin serving traffic.

A new Kubernetes cluster joins the same logical system only through an
explicitly designed placement and routing boundary. Cross-cluster failover and
consistency are not implied by deploying another cluster and require a separate
architecture decision.

## Observability boundary

Components emit structured logs and expose bounded-cardinality metrics. The
deployment layer may connect those signals to Prometheus, Grafana, or an
OpenTelemetry-compatible collector without coupling core correctness to a
specific vendor. Collection, generation, shard, replica, node, request, and
epoch identifiers belong in diagnostic context, but high-cardinality values
must not become unrestricted metric labels.

## Non-goals

Without a separate architecture decision, Veclet does not add a custom ANN
algorithm, consensus protocol, write-ahead log, query language, per-collection
controller, or controller proxy on the search path.
