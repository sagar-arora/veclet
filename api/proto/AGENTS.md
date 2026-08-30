# Protobuf and RPC review instructions

These instructions apply to `api/proto/` and extend the repository instructions.

## Contract design

- Keep the package versioned as `veclet.v1` until an explicit version transition is approved.
- Add only contracts required by an implemented behavior. Do not expose internal FAISS, RocksDB, Kubernetes, or C++ types.
- Prefer small request/response messages with explicit validation rules and documented units.
- Define dimension, cardinality, byte-size, enum, and range limits at the boundary.
- Distinguish resource identity from display names and document normalization rules.
- Define score semantics for each metric and whether higher or lower is better.
- Define strict versus partial query results explicitly; never imply success when a shard result is absent.

## Compatibility

- Never reuse field numbers or removed enum values. Reserve removed numbers and names.
- Do not change the meaning, units, signedness, or presence semantics of an existing field in place.
- Review renames for generated-language API impact even when wire encoding is unchanged.
- Use additive evolution where possible and provide a migration note for every incompatible change.
- Keep generated code out of hand-authored review comments; review the source proto and generation configuration.

## RPC behavior

- Document idempotency and retry safety for every RPC.
- Require deadlines and cancellation propagation in all clients and servers.
- Bound streaming history, reconnect behavior, message sizes, batch sizes, and fan-out.
- Include generation/epoch/version fields wherever stale work must be fenced.
- Use standard gRPC health and status codes consistently; preserve structured causes at process boundaries.

## Required checks

- Run Buf format/lint and the configured breaking-change check.
- Compile both Go and C++ generated bindings.
- Test invalid boundaries, unknown enum values, missing required semantic fields, retry duplication, and stale epoch/version behavior.
