# 0001: V1 protobuf contract

- Status: accepted
- Date: 2026-08-29

## Context

The first distributed slice needs a stable boundary from a client to a
stateless QueryNode and from that QueryNode to DataNodes. Adding controller,
build, streaming, and operational APIs now would commit behavior that has not
been implemented or tested.

## Decision

Use the versioned `veclet.v1` package and Buf's STANDARD lint plus FILE breaking
policy. The first contract contains one strict public search, one shard search,
one insert-only batch mutation, and a collection descriptor.

V1 fixes these behaviors:

- every successful public search includes all logical shards from one
  generation and routing epoch;
- L2 preserves lower-is-better squared distance, while inner product and cosine
  are higher-is-better;
- equal scores use ascending vector ID as the deterministic tie-breaker;
- generation and assignment epochs fence DataNode calls;
- requests are deadline-bound, messages and fan-out inputs are bounded, and
  exact `ApplyBatch` replay is idempotent;
- collection identity is canonical and distinct from mutable display text;
- omitted `logical_shards` resolves to 1, while explicit zero remains invalid.

Validation remains implementation-owned until generated bindings exist, so the
contract adds no annotation dependency. The validation matrix in
[`api/proto/README.md`](../../api/proto/README.md) is normative.

## Consequences

There is no partial-result mode, metadata return, update/delete mutation,
collection-management service, or implementation-specific index type in this
change. These can be added compatibly when their behavior is implemented.

Released fields cannot change meaning, units, signedness, or presence semantics
in place. Removed numbers and names are reserved forever. An incompatible
change requires a migration note and an explicit version transition rather
than weakening the configured breaking check.
