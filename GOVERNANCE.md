# Governance

Veclet currently uses a maintainer-led governance model while the initial
architecture and public contracts are established.

## Roles

- **Contributors** propose issues, documentation, code, tests, and reviews.
- **Reviewers** provide domain-specific review and may approve changes in their
  demonstrated area of expertise.
- **Maintainers** manage releases, repository settings, final merge decisions,
  and the project's architectural coherence.

Repository ownership is recorded in `.github/CODEOWNERS`. Ownership provides a
review route, not permission to bypass the same correctness and compatibility
standards applied to other contributors.

## Decisions

Routine decisions are made in issues and pull requests. Material changes to
service boundaries, authority, durability, compatibility, or operational
requirements require a written architecture decision before implementation.

Maintainers seek technical consensus. When consensus is not possible, they
record the decision and its tradeoffs in the relevant issue, pull request, or
architecture decision. Security reports follow `SECURITY.md` rather than public
discussion.

## Changes to governance

Governance changes use the normal pull-request process and require maintainer
approval. This model may evolve as the maintainer and reviewer community grows.
