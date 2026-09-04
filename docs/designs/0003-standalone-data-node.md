# 0003: Standalone DataNode process

- Status: implemented
- Date: 2026-09-04

## Intent

Provide the first runnable Veclet process: one `veclet-datad` executable that
opens one local shard, recovers RocksDB-authoritative records into a derived
FAISS HNSW index, installs one local `ShardPlacement`, and serves the existing
`DataService` RPCs over gRPC.

This is a standalone bootstrap path, not a second data plane. A future
controller placement stream will create the same `LocalShard` and
`ShardPlacement` objects inside the same server process.

## Non-goals

This slice does not add controller registration, placement streaming, multiple
local shards, replication, TLS, YAML parsing, Docker packaging, metrics, IVF
training artifacts, or a public client API. It does not make the internal
DataService safe to expose to an untrusted network.

## Configuration

The initial executable uses dependency-free `--name=value` flags. Required
values are explicit so a typo cannot silently create a different database:

```sh
veclet-datad \
  --data-directory=/var/lib/veclet \
  --collection-id=products \
  --dimension=384 \
  --metric=cosine
```

Optional flags and defaults are:

| Flag | Default | Rule |
| --- | --- | --- |
| `--listen-address` | `127.0.0.1:7402` | Loopback IPv4 or `localhost`, port 1–65,535; port 0 is test-only |
| `--shard-id` | `0` | Unsigned 32-bit logical-shard ID |
| `--generation-id` | `1` | Positive unsigned 64-bit generation |
| `--placement-epoch` | `1` | Positive unsigned 64-bit placement fence |
| `--index` | `hnsw` | HNSW is the only standalone bootstrap index in this slice |
| `--hnsw-m` | `32` | Positive integer, at most 65,536 |
| `--hnsw-ef-search` | `64` | Positive integer, at most 65,536 |

`dimension` is 1–65,536. `metric` is one of `l2`, `inner-product`, or
`cosine`. Collection identity follows `[a-z][a-z0-9-]{0,62}`. Every flag may
appear at most once; unknown flags and positional arguments fail startup.

`data-directory` is the base directory, not a reusable RocksDB path. The
process derives an identity-safe local path:

```text
<data-directory>/collections/<collection>/generation-<generation>/
  shard-<shard>/dimension-<dimension>/metric-<metric>/rocksdb
```

Collection, generation, shard, dimension, and metric therefore cannot
silently alias another standalone shard's records. Placement epoch and HNSW
search/build tuning are not part of the path because they do not change the
authoritative record set.

The loopback restriction is deliberate. Standalone v1 uses insecure gRPC and
must not accidentally listen on a public or VPC interface. Cluster listeners
require the later authenticated workload-identity and TLS design. Deployment
tools may eventually render these same validated values from YAML without
making a YAML parser part of the engine.

## Ownership and lifecycle

`StandaloneDataNode` owns, in destruction order, the gRPC server,
`DataNodeService`, shared `LocalShard`, and its validated configuration. It
starts no application-owned background thread. gRPC owns its internal server
workers; the process shuts the server down and waits for them before destroying
the service or shard.

Startup and shutdown use this state model:

| State | Listening | Health | Allowed transition |
| --- | --- | --- | --- |
| validating | no | unavailable | validate all configuration |
| recovering | no | unavailable | open RocksDB and rebuild FAISS |
| placement installed | no | unavailable | install the exact local placement |
| serving | yes | overall and `veclet.v1.DataService` are `SERVING` | accept bounded RPCs |
| shutting down | draining | `NOT_SERVING` | stop accepting work and wait for synchronous RPCs |
| stopped | no | unavailable | destroy service and local shard |

Configuration failure, corrupt RocksDB, recovery failure, placement failure,
or listen failure aborts startup before readiness. `BuildAndStart` succeeds only
after recovery and placement installation, so the server never advertises a
shard that is still rebuilding.

The standard gRPC health service is enabled. An empty service name represents
overall process readiness; `veclet.v1.DataService` represents the data service.
Both become serving only after the server has a recovered active placement and
become not serving before shutdown begins.

The executable blocks `SIGINT` and `SIGTERM` before gRPC creates worker threads.
The main thread waits synchronously for either signal and then initiates
idempotent server shutdown. No application-owned signal thread, detached
thread, or polling sleep is used.

## RPC and storage boundaries

The server sets both inbound and outbound gRPC message limits to 4 MiB. Clients
still own deadlines; the synchronous handlers check cancellation before and
after bounded local work as defined by Design 0002.

RocksDB remains authoritative. HNSW is reconstructed from RocksDB on every
restart in this slice. Because HNSW requires no training artifact, it is the
only safe production ANN bootstrap choice until immutable IVF training and
generation artifacts exist.

## Observability and security

Startup, readiness, shutdown, and fatal startup errors are emitted to standard
error with collection, generation, shard, and placement context. Request-level
structured logging and bounded-cardinality metrics are separate observability
work; payloads, embeddings, and credentials must never be logged.

The listener is insecure and loopback-only. There are no cloud credentials,
join tokens, or network identity fields in this configuration.

## Acceptance criteria

- Invalid, incomplete, duplicate, unknown, or unsafe listener configuration
  fails before RocksDB is opened.
- A real TCP client can insert and search through the running server.
- The standard gRPC health service reports the overall server and
  `veclet.v1.DataService` as serving.
- Shutdown is idempotent and completes all owned thread and server lifecycles.
- Restarting on the same data directory recovers committed records and makes
  them searchable before readiness.
- The executable and tests add no production dependency.
