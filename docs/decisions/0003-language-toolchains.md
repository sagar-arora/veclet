# 0003: Initial language toolchains

Status: accepted

## Context

The protobuf source contract must compile in both languages before runtime code
can depend on it. Generation must be reproducible, must not depend on an
unversioned executable, and must not disclose private schemas to a remote code
generation service.

The initial supported environments are macOS arm64 for local development and
Ubuntu 24.04 amd64 for CI. Windows and cross-compilation are not yet supported.

## Decision

- Go uses modules, Go 1.26.7, `protoc-gen-go` 1.36.12, and
  `protoc-gen-go-grpc` 1.6.2. The generated runtime dependencies are Protobuf Go
  1.36.12 and gRPC-Go 1.83.2.
- C++ uses C++20, CMake 3.28 or newer, Ninja 1.11 or newer, and vcpkg manifest
  mode. The registry baseline is the immutable vcpkg `2026.05.25` release at
  `d015e31e90838a4c9dfa3eed45979bc70d9357fc`.
- That baseline resolves gRPC C++ 1.76.0#1, Protobuf 6.33.4#2, and GoogleTest
  1.17.0#2. Port revisions are part of the baseline and are not floated.
- Buf 1.72.0 invokes only local generators. Go plugins are module-pinned tools;
  C++ uses `protoc` and `grpc_cpp_plugin` from the same vcpkg graph as the
  linked runtime.
- Generated Go and C++ files are build output under ignored directories. They
  are never edited or committed. CI regenerates them from a clean checkout.
- CI actions use immutable commit SHAs, read-only repository permissions,
  explicit operating-system versions, and bounded timeouts.

Compiler support begins with Apple Clang 17 on macOS arm64 and GCC 13 on Ubuntu
24.04 amd64. Using a newer compiler is allowed when it preserves the C++20
contract; dependency and generator versions remain pinned until an intentional
upgrade changes this decision.

## Consequences

The Go and C++ bindings cannot drift from the protobuf sources or their pinned
generators. C++ code generation and compilation use one dependency graph, which
avoids protoc/runtime version skew. A first C++ configure downloads and builds
the pinned vcpkg graph, so it is slower than subsequent cached builds.

Changing Go, Buf, vcpkg, Protobuf, gRPC, CMake minimums, or the supported
platforms is an explicit dependency change with clean generation and build
evidence. No runtime service, thread, network call, or durable state is added by
this decision.
