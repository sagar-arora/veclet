## What and why

<!-- Describe one reviewable behavior and why it is needed. -->

## Non-goals

<!-- State what this PR deliberately does not change. -->

## Acceptance criteria

- [ ] The intended behavior is externally observable or documented.
- [ ] Invalid input and credible failure paths are covered where applicable.
- [ ] No unrelated refactoring, dependency upgrade, or generated churn is mixed in.

## Architecture and compatibility

<!-- List affected invariants, state transitions, crash windows, and compatibility concerns. Use "None" only with a short reason. -->

## Review lanes

- [ ] Distributed systems
- [ ] Protobuf/API
- [ ] Go
- [ ] C++
- [ ] Tests/reliability
- [ ] Operations/security
- [ ] Documentation/governance only

## Verification

<!-- List exact commands and results, plus checks that could not run. -->

## Checklist

- [ ] I read the root and applicable scoped `AGENTS.md` files.
- [ ] New dependencies are necessary, explained, and pinned.
- [ ] Network calls have deadlines, cancellation, and bounded retry behavior.
- [ ] Background work has explicit lifecycle ownership and shutdown behavior.
- [ ] Logs and metrics avoid secrets and unbounded-cardinality labels.
- [ ] Public contracts, state transitions, and operational behavior are documented.
