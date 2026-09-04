# Veclet

Veclet is an early-stage distributed vector database built around FAISS. It aims
to keep the path from gRPC to vector search small, explicit, and operable in
both standalone and Kubernetes deployments.

Veclet is not ready for production use. Its public contracts and build system
are still being established.

## Design principles

- FAISS owns approximate-nearest-neighbor behavior.
- RocksDB is authoritative local record state; FAISS indexes are derived.
- A Go control plane owns metadata, placement, routing, and recovery.
- A C++ data plane owns vector persistence and search.
- QueryNodes are stateless and scale independently of collections and shards.
- Standalone and clustered deployments use the same protobuf contracts and
  data plane.
- Kubernetes and service-mesh integrations remain optional orchestration
  layers.
- Deployment simplicity is a product feature: one developer should be able to
  take one or three ordinary Linux VMs from infrastructure-ready to a working
  insert-and-search service within minutes, using the same documented workflow
  on AWS, GCP, Azure, Oracle Cloud, or bare metal.
- Opinionated deployment profiles target workloads in the tens of millions of
  vectors and provide a clear path toward roughly 100 million, without forcing
  users to operate a large collection of supporting systems. Published sizing
  always states vector dimension, index type, replication, memory, disk, and
  workload assumptions.

Veclet is intended for indie developers and small teams that need a production-
shaped vector service but do not want to become operators of a complex data
platform before they can evaluate or ship their application.

See [the architecture overview](docs/architecture.md) for the component and
data model, [the design index](docs/designs/README.md) for detailed proposals,
and [CONTRIBUTING.md](CONTRIBUTING.md) before proposing a change.

## Development

Run `make help` from the repository root to list the available commands. The
foundation checks require only Git, GNU Make, and Bash; see
[the development guide](docs/development.md) for their contract and CI wiring.

## Repository layout

| Path | Responsibility |
| --- | --- |
| `api/proto/` | Versioned protobuf and gRPC contracts |
| `control/` | Go controller and cluster metadata |
| `engine/` | Native QueryNode, DataNode, FAISS, and RocksDB engine |
| `ingest/` | Optional Go ingestion integrations |
| `deploy/` | Standalone and Kubernetes deployment assets |
| `tests/` | Cross-component integration and reliability tests |

## License

Apache License 2.0. See [LICENSE](LICENSE).
