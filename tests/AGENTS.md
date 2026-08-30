# Reliability and test review instructions

These instructions apply to `tests/` and extend the repository instructions.

## Test design

- Use fixed seeds and record enough input to reproduce a failure.
- Do not use wall-clock sleeps to coordinate deterministic tests. Use fake clocks, barriers, readiness probes, latches, channels, or failpoints.
- Put a bound on every wait and print useful state on timeout.
- Test externally observable contracts rather than private implementation details unless the test is for a state machine or deterministic utility.
- Keep fixtures small and explicit; do not hide expected topology, metric, generation, or replication factor in helpers.

## Correctness suites

- ANN quality uses recall-at-K against a test-only exact Flat oracle with documented thresholds and parameters.
- Global top-K merge uses exact known shard-local results and covers metric direction, ties, duplicate IDs, fewer-than-K results, and empty shards.
- State-machine tests cover every transition plus invalid and repeated transitions.
- Compatibility tests retain prior protobuf descriptors and artifact fixtures when formats become released contracts.

## Failure matrix

Before claiming resilience, cover relevant points from this matrix:

- QueryNode termination and restart;
- DataNode termination at RF=1 and RF=2;
- controller unavailability and restart;
- RPC timeout, cancellation, duplicate, delay, and partial response;
- stale routing, generation, lease, and assignment epochs;
- crash before/after RocksDB write, FAISS mutation, artifact publish, routing publish, replica acknowledgement, and Kafka offset commit;
- corrupt, missing, partial, or incompatible artifacts;
- drain and scale-in during active traffic or recovery.

## Review evidence

- A failure test must prove it exercised the intended fault, not merely that the final assertion passed.
- Avoid broad retry loops that can mask races or eventual-consistency bugs.
- Separate slow integration and E2E suites from unit tests, but keep a documented local command for each.
- Quarantine is not a fix. A flaky test needs a tracked root-cause issue, owner, and removal deadline.
