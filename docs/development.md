# Development

The foundation checks require Git, GNU Make, and Bash. Protobuf checks require
Buf 1.72.0. They run on macOS for local development and Linux in CI.

## Root commands

Run `make help` to see every command currently available from the repository
root.

| Command | Purpose |
| --- | --- |
| `make check-docs` | Validate that local Markdown link targets exist. |
| `make check-proto` | Verify the pinned Buf version, formatting, lint, and source build. |
| `make check-proto-breaking` | Compare protobuf compatibility against local `main`. |
| `make check-repository` | Reject missing structural files, trailing whitespace, and tracked build artifacts. |
| `make check` | Run all checks available in the checkout. |
| `make ci` | Run the same aggregate checks used by CI. |

The root Makefile is an entry point, not a replacement for language-native
build systems. Protobuf, Go, and C++ targets will be added only when their Buf,
Go module, and CMake manifests exist; each target will delegate to those tools.
This keeps missing build wiring visible instead of representing placeholder
jobs as successful builds.

## CI contract

Pull requests and pushes to `main` run independent documentation and repository
hygiene jobs from a clean checkout. Jobs use read-only repository permissions,
bounded timeouts, and concurrency cancellation for superseded revisions.

Language compilation, formatting, linting, and tests become required alongside
the change that introduces the owning toolchain. No CI job should claim a
component passed before that component can actually build.
