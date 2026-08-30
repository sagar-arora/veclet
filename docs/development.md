# Development

The foundation checks require Git, GNU Make, and Bash. Protobuf checks require
Buf 1.72.0. Go checks require Go 1.26.7. C++ checks require CMake 3.28 or
newer, Ninja 1.11 or newer, and the pinned vcpkg checkout. Local development is
supported on macOS arm64; CI uses Ubuntu 24.04 amd64.

## Root commands

Run `make help` to see every command currently available from the repository
root.

| Command | Purpose |
| --- | --- |
| `make check-docs` | Validate that local Markdown link targets exist. |
| `make generate-go` | Generate ignored Go protobuf and gRPC bindings with module-pinned local plugins. |
| `make check-go` | Regenerate, format-check, vet, and test the Go bindings. |
| `make check-cpp` | Configure with vcpkg, generate local C++ bindings, build with Ninja, and run CTest. |
| `make check-proto` | Verify the pinned Buf version, formatting, lint, and source build. |
| `make check-proto-breaking` | Compare protobuf compatibility against local `main`. |
| `make check-repository` | Reject missing structural files, trailing whitespace, and tracked build artifacts. |
| `make check` | Run all checks available in the checkout. |
| `make ci` | Run the same aggregate checks used by CI. |

The root Makefile is an entry point, not a replacement for language-native
build systems. It delegates to Buf, Go modules, and CMake presets so the same
commands work locally and in CI.

## Local setup

Clone the exact vcpkg registry baseline and bootstrap it once:

```sh
git clone https://github.com/microsoft/vcpkg.git .cache/vcpkg
git -C .cache/vcpkg checkout --detach d015e31e90838a4c9dfa3eed45979bc70d9357fc
.cache/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT="$PWD/.cache/vcpkg"
```

The checkout is local tooling and remains ignored. `make check-cpp` installs
the manifest into `build/cpp/vcpkg_installed`, generates bindings with the
vcpkg-provided protoc and gRPC plugin, builds with Ninja, and runs CTest.

`make generate-go` invokes the generators declared by `go.mod`. It does not
require globally installed protoc plugins. Both language generators run
locally; Veclet schemas are not uploaded to a remote generation service.

Generated files live under `gen/` or `build/` and are deliberately untracked.
Delete those directories whenever a fully clean regeneration is needed.

## CI contract

Pull requests and pushes to `main` run independent documentation and repository
hygiene jobs from a clean checkout. Jobs use read-only repository permissions,
bounded timeouts, and concurrency cancellation for superseded revisions.

Language compilation, formatting, linting, and tests are required alongside
the change that introduces the owning toolchain. The Go and C++ jobs regenerate
from source in clean checkouts before compiling or testing.

Dependency and supported-platform choices are recorded in
[the initial language-toolchain decision](decisions/0003-language-toolchains.md).
