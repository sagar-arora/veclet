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

See [the architecture overview](docs/architecture.md) for the component and
data model, and [CONTRIBUTING.md](CONTRIBUTING.md) before proposing a change.

## Development

Run `make help` from the repository root to list the available commands. The
foundation checks require only Git, GNU Make, and Bash; see
[the development guide](docs/development.md) for their contract and CI wiring.

## Repository layout

| Path | Responsibility |
| --- | --- |
| `api/proto/` | Versioned protobuf and gRPC contracts |
| `control/` | Go controller and cluster metadata |
| `cpp/` | C++ QueryNode, DataNode, FAISS, and RocksDB code |
| `ingest/` | Optional Go ingestion integrations |
| `deploy/` | Standalone and Kubernetes deployment assets |
| `tests/` | Cross-component integration and reliability tests |

## License

Apache License 2.0. See [LICENSE](LICENSE).
