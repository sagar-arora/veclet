# C++ data-plane review instructions

These instructions apply to `engine/` and C++ executables and extend the repository instructions.

## Language and ownership

- Use repository-pinned C++20, compiler, CMake, Ninja, and dependency versions.
- Prefer value semantics and RAII. Express ownership with values or smart pointers; raw pointers and references are non-owning.
- Do not use detached threads. Every thread, completion queue, callback, and background task has a stop signal and join path.
- Avoid global mutable state and static initialization with cross-translation-unit ordering requirements.
- Keep headers self-contained, minimize public surface, use `const` deliberately, and prefer `std::span`/views for non-owning buffers.
- Do not let exceptions cross gRPC, C ABI, thread-entry, or process boundaries. Translate failures once with preserved context.
- Check integer conversions, vector dimensions, byte sizes, alignment, overflow, and lifetimes at RPC and storage boundaries.
- Do not hand-edit generated protobuf or gRPC sources.

## Concurrency and RPC

- Document thread safety for every shared object, especially FAISS indexes, RocksDB handles, shard maps, and caches.
- Define lock ordering and never hold a mutex across blocking RPC, filesystem, RocksDB, or object-store work.
- Propagate deadlines and cancellation into asynchronous gRPC work and stop issuing shard calls after cancellation.
- Bound completion queues, worker pools, batches, fan-out, result buffers, and retry attempts.
- Keep callback captures lifetime-safe; reject use-after-free risks during shutdown or cancellation.

## FAISS and RocksDB correctness

- Use FAISS for ANN, training, metrics, IDs, and serialization; do not reimplement its algorithms.
- Validate IVF training state before add/search and document HNSW deletion limitations.
- Normalize cosine vectors in one documented layer and test both stored and query vectors.
- Keep L2 and inner-product score ordering distinct and make global top-K tie-breaking deterministic.
- Use a test-only Flat index as the exact oracle; do not expose Flat as the normal production index.
- Treat RocksDB as authoritative. Every combined RocksDB/FAISS mutation needs a documented crash window and recovery path.
- Pin FAISS serialization compatibility and verify artifact checksums before load or activation.

## Required checks

- Repository clang-format and clang-tidy configuration.
- CMake/Ninja build and GoogleTest/CTest.
- Address/undefined behavior sanitizers for owned code; thread sanitizer for focused concurrency suites where supported.
- Deterministic tests for empty input, invalid dimensions, NaN/Inf policy, duplicate IDs, stale versions, cancellation, shutdown, restart, and corrupted artifacts.
