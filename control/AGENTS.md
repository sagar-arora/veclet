# Go controller review instructions

These instructions apply to `control/` and extend the repository instructions.

## Controller boundaries

- The controller owns collection metadata, node health, placement, routing, generations, builds, and recovery orchestration.
- It must not store vectors, execute FAISS search, or proxy steady-state QueryNode-to-DataNode traffic.
- Keep `MetadataStore` private to the controller. QueryNodes and DataNodes must not depend on SQLite or Kubernetes storage details.
- Placement decisions are deterministic for identical inputs and make anti-affinity or capacity assumptions explicit.
- Routing publication is monotonic and includes only READY replicas.

## Go standards

- Use the Go version pinned by the repository; do not rely on a newer local toolchain feature.
- Pass `context.Context` explicitly as the first parameter for request-scoped work. Do not store request contexts in structs.
- Wrap errors with operation and stable identifiers using `%w`; use `errors.Is`/`errors.As` for classification.
- Prefer small concrete types and consumer-owned interfaces. Do not add interfaces solely to mirror implementations.
- Avoid package globals and hidden singletons. Inject clocks, stores, clients, and ID sources needed for deterministic tests.
- Every goroutine has one documented owner, shutdown signal, and join/wait path. No fire-and-forget goroutines.
- Define channel ownership and closure. Bound worker pools, queues, retries, and watch reconnect loops.
- Guard shared state deliberately; document mutex ownership and never hold locks across network or storage calls.
- Use structured logging with stable keys and avoid secrets or unbounded-cardinality values.
- Keep `internal/` package boundaries aligned with capabilities rather than technical layers that create cycles.

## Review focus

- Simulate duplicate registration, missed/late heartbeat, stale lease, controller restart, watch reconnect, concurrent collection updates, and drain during recovery.
- Require fake-clock tests for leases, timeouts, scheduling, and backoff.
- Run unit tests with the race detector where supported.
- Reject reconciliation loops that are non-idempotent, busy-spin, or can create duplicate work after restart.
- Preserve a last-known-good routing snapshot during temporary controller unavailability.

## Required checks

- `gofmt`, `go vet`, configured `golangci-lint`, and `go test ./...`.
- Race-enabled tests for packages that own concurrency or watches.
- Deterministic placement and state-transition tests using fixed inputs and fake time.
