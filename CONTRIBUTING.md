# Contributing to Veclet

Veclet is in its foundation stage. Small, reviewable changes are preferred over
large framework or abstraction PRs.

## Before opening a change

1. Read `AGENTS.md` and the closest scoped `AGENTS.md` for the files you will
   change.
2. State the intended behavior, non-goals, acceptance criteria, and affected
   architecture invariants in the issue or PR.
3. For distributed state, document ownership, transitions, crash windows,
   retry safety, and recovery before implementation.
4. Avoid combining feature behavior, refactoring, dependency upgrades, and
   generated-code churn.

Use a short branch name such as `proto/search-contract` or
`control/collection-store`. Do not push feature work directly to `main`.

## Testing and compatibility

Functional changes must cover success, invalid input, and credible failure
paths. State-machine changes need a transition table or diagram. Distributed
claims require deterministic failure evidence, not only a passing happy path.

Preserve protobuf field numbers, persisted artifacts, and public API behavior.
Removed protobuf numbers and names must be reserved. Generated files are never
edited by hand.

Component-specific format, lint, build, and test commands will be documented as
their build systems land. Until then, a PR must list every check actually run
and every check that could not run.

## Review

The PR template identifies independent review lanes for distributed systems,
protobuf/API, Go, C++, tests/reliability, and operations/security. A change may
require more than one lane. Review findings should include a concrete failure
scenario, impact, and safe correction.

All contributions must comply with the [Code of Conduct](CODE_OF_CONDUCT.md)
and are accepted under the [Apache License 2.0](LICENSE).
