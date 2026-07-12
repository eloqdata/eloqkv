---
name: finish-pr
description: Use when a non-trivial code change is substantially complete and needs a final diff audit, documentation check, verification record, or pull request description.
---

# Finish PR

Prepare reviewer-facing delivery from repository evidence.

## Workflow

1. Read `AGENTS.md`, `CLAUDE.md`, and `.github/pull_request_template.md`.
2. Determine the PR base branch or repository default branch. Inspect `git status`,
   commits since the merge base, the complete merge-base diff, and important files
   in context. Include submodule pointer changes; do not treat only unstaged files
   as the change.
3. Audit comments in changed code. Add or update only comments that explain
   non-obvious invariants, concurrency, ownership/lifetime, ordering, durability,
   recovery/retry, performance, or compatibility. Do not narrate syntax.
4. Run the relevant build, tests, formatting, and documentation checks from
   `CLAUDE.md`. Record exact commands and outcomes. State checks not run.
5. Fill the repository PR template from the final diff.

## Quality gate

- Describe concrete behavior and named components; avoid “updated logic.”
- Explain material decisions and tradeoffs without inventing rejected designs.
- Cover compatibility, regression/operational risk, rollback, and the best review
  entry points.
- Verify every claim against the final diff and current command output.
- Keep trivial changes concise; make non-trivial changes complete.
- Draft only unless the user explicitly asks to publish or update the PR.
