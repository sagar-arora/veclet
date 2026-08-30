# Deployment and operations review instructions

These instructions apply to `deploy/` and extend the repository instructions.

## Deployment boundaries

- Kubernetes and Istio change orchestration and networking only; they do not own vector routing, shard placement, or database state transitions.
- Keep standalone mode functional without cluster dependencies.
- Keep the runtime and primary configuration cloud-neutral. Provider-specific assets must be thin adapters around the same processes and contracts.
- QueryNodes use Deployments without persistent volumes. DataNode storage and drain behavior must be explicit.
- Do not autoscale DataNodes until tested replica movement and safe scale-in exist.

## Deployment experience

- Maintain an opinionated single-node evaluation profile and a three-node production-shaped profile.
- Target infrastructure-ready to first successful insert and search within minutes; test and document the measured path rather than relying on an aspirational quickstart.
- Prefer one generated configuration, preflight command, startup command, and readiness command over provider-specific manual sequences.
- Keep AWS, GCP, Azure, Oracle Cloud, Kubernetes, and bare-metal modules optional and behaviorally equivalent at the Veclet boundary.
- Publish capacity profiles for representative 10-million, 20-million, and approximately 100-million-vector datasets. Include dimension, index parameters, replication, CPU, RAM, disk, query and ingest load, and recovery headroom.
- Fail preflight when disk, memory, ports, identity, or replication settings are obviously unsafe. Do not silently choose a configuration that risks data loss.

## Kubernetes review

- Pin images by immutable version or digest in release artifacts; never rely on `latest`.
- Set resource requests/limits intentionally and make probes reflect real liveness/readiness semantics.
- Align termination grace periods, preStop/drain behavior, PodDisruptionBudgets, and rollout strategy with replica safety.
- Use least-privilege RBAC and workload identity. Never place raw cloud credentials or secrets in protobuf, images, logs, or committed values.
- Keep CRDs small and user-facing; do not turn every internal object into a CRD.
- Review upgrade, downgrade, rollback, and CRD compatibility before changing released fields.

## Istio and networking review

- Keep application-level retries shard-aware. Mesh retries must not duplicate non-idempotent writes or expand the end-to-end deadline.
- Document ownership of timeouts and retries so application and mesh policies do not multiply attempts.
- Use strict mTLS in cluster profiles only after direct-network behavior is proven.
- Preserve request IDs and trace context through gateway, QueryNode, and DataNode hops.

## Required checks

- Render and validate Helm/Kubernetes manifests in CI.
- Run policy/security checks and reject privileged or overly broad capabilities without explicit justification.
- Use kind E2E for create, search, QueryNode scale, DataNode add/drain, rolling update, and builder Job behavior.
- Prove disruptions exercise the intended pod/node condition and verify replication before allowing termination.
