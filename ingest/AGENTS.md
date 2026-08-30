# Go ingestion review instructions

These instructions apply to `ingest/` and extend the repository instructions.

## Ingestion contract

- Kafka is an optional durable upstream log; `ingestd` does not become the authoritative database of record.
- Route mutations deterministically by logical shard, never by physical node.
- Commit an offset only after the configured durable acknowledgement policy is satisfied.
- Preserve required ordering within a Kafka partition and document behavior across partitions.
- Treat duplicate delivery as normal. Version checks and mutation application must be idempotent.
- Rebalances, shutdown, cancellation, backpressure, and retry exhaustion must not silently lose acknowledged records.

## Go and concurrency standards

- Follow the controller Go standards for contexts, error wrapping, injected dependencies, structured logs, and lifecycle ownership.
- Bound consumers, per-shard queues, in-flight RPCs, batch sizes, retry attempts, and memory.
- Do not launch one unbounded goroutine per message.
- Do not hold partition progress hostage to unrelated shards without an explicit bounded policy.
- Stop fetching, drain or cancel in-flight work according to the documented shutdown protocol, then commit only safe offsets.

## Review focus

- Test crashes before and after RocksDB durability, replica acknowledgement, and Kafka offset commit.
- Test duplicate events, stale versions, poison records, partial replica failure, retry exhaustion, rebalance, and restart.
- Require metrics for lag, in-flight records, retries, failed mutations, acknowledgement latency, and last committed offsets with bounded labels.
- Keep UPSERT, DELETE, delta-index, and compaction semantics out of the insert-only stage until separately approved.
