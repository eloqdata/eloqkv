---
name: respond-to-review
description: Use when pull request review comments, requested changes, or reviewer questions need to be investigated, addressed, or answered.
---

# Respond to Review

Resolve feedback against the current code and report exactly what changed.

## Workflow

1. Read repository instructions and collect every in-scope review comment with
   its file, line, author, and thread status.
2. Inspect the referenced code, callers, tests, and relevant diff before deciding
   whether the feedback is valid. Do not implement comments blindly.
3. Classify each item as accepted, already addressed, technically inapplicable,
   or requiring user/product direction. Give evidence for disagreements.
4. For accepted items, make the smallest root-cause fix, update necessary
   explanatory comments/docs, and run focused regression checks plus required
   repository checks.
5. Re-read the final diff and draft a response that maps each review item to the
   concrete change and verification result. Call out unresolved items and unrun
   checks explicitly.

Do not resolve threads, post comments, push, or update a PR unless the user asks.
