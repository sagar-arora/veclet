# Veclet agent instructions

## Scope

These instructions apply repository-wide. Read the closest nested `AGENTS.md` for the files in scope before changing code.

Veclet is a minimal distributed vector database built around FAISS. Keep the architecture understandable enough to trace a request from gRPC to FAISS without crossing unnecessary coordinators or abstractions.

## Architecture invariants

- Logical shard and physical DataNode are never the same abstraction.
- QueryNodes have no durable state and can be added or removed without data movement.
- The controller does not serve or proxy steady-state searches.
- FAISS owns ANN behavior; do not implement custom ANN algorithms or distance functions.
- RocksDB is authoritative local record state; FAISS is a derived search structure.
- Routing contains only READY replicas.
- A replica is not removed or drained until the required replication factor is safe.
- Generation artifacts are immutable and activate atomically.
- Source partitions are ingestion concepts and never query-routing concepts.
- Standalone mode does not require Kubernetes, Istio, Kafka, Airflow, or an object-store service.
- Standalone and clustered modes use the same protobuf contracts and C++ data plane.
- Deployment simplicity is a product invariant: maintain documented single-node and three-node paths that can reach a working insert and search within minutes after ordinary Linux infrastructure is ready.
- Core configuration and runtime behavior remain cloud-neutral. AWS, GCP, Azure, Oracle Cloud, Kubernetes, and bare-metal integrations are optional provisioning or orchestration adapters.
- Capacity claims must state vector dimension, index configuration, replication, CPU, RAM, disk, workload, and recovery assumptions; never imply that a record count alone guarantees a safe deployment.
- Do not add a service, coordinator, metadata system, query language, custom WAL, or custom consensus protocol without explicit approval and a written architecture decision.

## Working agreements

- Make the smallest change that produces one reviewable behavior.
- Do not mix refactoring, dependency upgrades, generated-code churn, and feature behavior in one change.
- Do not add a production dependency when the standard library or an existing dependency is sufficient. Explain and pin every new dependency.
- Preserve protobuf, persisted-artifact, and public API compatibility within a minor release unless the change explicitly declares a break.
- Never reuse a protobuf field number. Reserve removed fields and names.
- Every functional change includes tests for success, invalid input, and credible failure paths.
- Every state machine change includes a transition table or diagram in the same PR.
- Every network call has a deadline, propagates cancellation, and has a bounded retry budget.
- Every goroutine, thread, task, callback, stream, and background loop has explicit lifecycle ownership and shutdown behavior.
- Do not use sleeps as deterministic test synchronization. Prefer fake clocks, latches, barriers, channels, and failpoints.
- Do not swallow or log-and-ignore errors. Add context while preserving the underlying cause.
- Do not leave a TODO unless it links to a tracked issue and explains the safety of deferral.
- Do not edit generated files by hand.
- Do not commit, push, open a PR, merge, publish, or modify branch protection unless the user explicitly authorizes that GitHub action.

## Required implementation workflow

1. State the intended behavior, non-goals, acceptance criteria, and affected invariants.
2. Inspect current code, tests, build files, and active instructions before editing.
3. For distributed state, write the ownership, transition, crash-window, retry, and recovery model first.
4. Implement the narrow behavior without unrelated cleanup.
5. Run the smallest relevant checks, then the owning component suite.
6. Review the diff for generated files, compatibility, dependency changes, and unrelated churn.
7. Report exact commands and results; call out checks that could not run.

## Independent review lanes

Before recommending merge, evaluate every applicable lane independently:

- distributed systems: all RPC, state, concurrency, routing, placement, replication, snapshot, recovery, and lifecycle changes;
- protobuf/API: changes under `api/proto/` or behavior visible across process boundaries;
- Go: changes under `control/` or `ingest/`;
- C++: changes under `cpp/` or C++ executables;
- tests/reliability: failure claims, concurrency, recovery, or nondeterministic behavior;
- operations/security: deployment, credentials, identity, networking policy, images, or resource lifecycle.

When separate reviewer agents are available and the user requests independent review, assign one reviewer per applicable lane. Give reviewers the diff and acceptance criteria, not the author’s conclusion. The implementation author must not provide the only approval.

## Code Review Rules — distributed systems

Review findings must identify a concrete failure scenario and the violated contract.

### Ownership and authority

- Flag state with multiple writers unless ordering, conflict resolution, and the authoritative owner are explicit.
- Flag derived state that can outlive or override newer authoritative state.
- Require epochs, versions, leases, or fencing tokens where a delayed actor could mutate a newer assignment or generation.
- Require deterministic placement for identical inputs and stable tie-breaking where ordering affects state.

### State machines and crash consistency

- Enumerate every transition, precondition, durable write, externally visible effect, and recovery action.
- Check crash windows between RocksDB writes, FAISS mutation, artifact publication, routing publication, offset commits, and acknowledgement.
- Flag transitions that are neither idempotent nor safely resumable.
- Reject activation or routing publication before all required artifacts/replicas are verified READY.

### Time, failure, and retries

- Treat RPC loss, delay, duplication, reordering, partial response, cancellation, and process restart as normal cases.
- Require one end-to-end deadline budget; nested retries must not reset or multiply it.
- Retry only operations proven idempotent and only an equivalent replica for the same shard.
- Flag heartbeat or lease logic that depends on wall-clock equality or cannot be tested with a fake clock.
- Require bounded queues, fan-out, buffers, reconnect loops, and backoff.

### Replication and routing

- Queries select one replica per shard; querying all replicas requires an explicit, reviewed reason.
- Routing updates must be monotonic, reconnectable, and safe when the controller is unavailable.
- Only READY replicas may receive traffic.
- Drain and scale-in must preserve replication factor before removing placement.
- Define whether unavailable shards fail the whole query or produce explicit partial results; never return silent partial success.

### Compatibility and observability

- Review protobuf, manifest, FAISS serialization, RocksDB keys, and checkpoint changes for forward/backward compatibility.
- Require bounded cardinality in metrics and structured identifiers in logs/traces.
- Failures must expose enough collection, generation, shard, replica, node, request, and epoch context to diagnose ownership and ordering.

### Test evidence

- Require deterministic unit tests for transition and placement logic.
- Require component tests for restart and local recovery.
- Require integration tests before availability, replication, or failover claims.
- Separate exact distributed merge correctness from approximate ANN recall thresholds.

## Review output format

- Findings first, ordered P0 through P3.
- Each finding includes the file and smallest useful line range, failure scenario, impact, and safe correction.
- Do not report formatting issues already enforced by CI.
- Do not approve based only on passing tests; identify missing tests for credible failure modes.
- If there are no findings, state that explicitly and list residual risks or checks not run.

## Definition of done

- Build, format, lint, and relevant tests pass.
- New behavior and failure paths are tested.
- Public behavior and state transitions are documented.
- No unrelated files changed.
- Architecture invariants remain intact.
- Applicable independent review lanes have no unresolved P0 or P1 findings.
