# EloqKV technical documentation

This directory is the current-architecture authority for the EloqKV API layer. It explains the stable system model needed to change the code safely; product usage and build instructions remain in the repository [README](../README.md), while transaction-engine internals belong to [`data_substrate/docs/`](../data_substrate/docs/README.md).

## Reading order and subsystem map

Read the overview first, then only the focused documents for the code in scope.

| Document | Current responsibility | Primary source |
|---|---|---|
| [01 — Architecture overview](01-architecture-overview.md) | Process boundary, component ownership, startup and end-to-end request flow | `src/redis_server.cpp`, `src/redis_service.cpp` |
| [02 — Command processing](02-command-processing.md) | RESP dispatch, connection and transaction state, engine request adaptation and replies | `src/redis_service.cpp`, `include/redis_connection_context.h` |
| [03 — Data model](03-data-model.md) | Redis keys, objects and commands exposed through the Data Substrate catalog contract | `include/redis_object.h`, `include/redis_command.h`, `include/eloqkv_catalog_factory.h` |
| [04 — Scripting, Pub/Sub and blocking commands](04-scripting-pubsub-blocking.md) | Feature-specific orchestration that spans connection state and engine requests | `src/lua_interpreter.cpp`, `src/pub_sub_manager.cpp`, `src/redis_command.cpp` |
| [05 — Namespaces](05-namespaces.md) | Namespace identity, key isolation, authentication and retired-prefix cleanup | `include/namespace/`, `src/namespace/` |
| [06 — Vector search](06-vector-search.md) | Vector-index ownership, transactional metadata and index recovery | `include/vector/`, `src/vector/` |
| [07 — Persistence interoperability and tools](07-persistence-and-tools.md) | Redis DUMP/RESTORE compatibility and offline RDB/AOF export boundaries | `src/redis_rdb_restore.cpp`, `src/tools/` |

The numbered documents deliberately follow the existing repository taxonomy. They are focused views of one system, not independent specifications; cross-cutting transaction, durability and clustering behavior is authoritative in [`data_substrate/docs/`](../data_substrate/docs/README.md).

## Architecture authoring standard

Architecture is a compact, present-tense model of the current core design. It covers durable module boundaries, primary control and data flows, ownership and lifecycles, durable or wire contracts, external integrations, system-level invariants, and stable rationale.

Freshness is claim-driven. For an implementation change, identify the existing claim or core model that would otherwise become false or materially incomplete. Update architecture only when the change crosses one of those boundaries. Dedicated documentation work may correct an inaccuracy, fill a core-design gap, or consolidate sediment.

When a durable module is introduced, removed, split, or merged, update this subsystem map and the narrowest focused documents in the same change. Keep the overview at system context, high-level flow, cross-cutting invariants, and navigation.

Revise related prose into one coherent current explanation. Do not append a change narrative. Write at the highest useful abstraction and route narrower information to its durable home:

| Information | Home |
|---|---|
| Core responsibility, ownership, lifecycle, cross-module flow, durable or wire contract, system invariant, stable rationale | These current-architecture documents |
| Significant historical decision, alternatives, superseded design or architectural evolution | An ADR or explicitly historical design document |
| Supported procedure, prerequisite or operational safety boundary | Operations or user documentation |
| Local algorithm, runtime representation, tuning mechanism or code-level invariant | Nearby source or API documentation |
| Change motivation, before/after behavior, one-off benchmark and implementation journey | Pull request or commit |

Every architecture document must end with a `Source map` table that maps its material claims to concrete repository-relative paths. Cite directories only when the claim genuinely spans the directory. Mark an unverified boundary as `Unknown; confirm before documenting` instead of inferring intent from a name. When code and documentation disagree, code is authoritative and the stale claim must be repaired.

Material under `proposals/` or `research/` records investigation or possible future design and is not authority for current behavior. `plans/` and `superpowers/`, if introduced, are likewise historical change context. Status must be explicit, and implemented design must be reconciled into the numbered architecture set instead of leaving historical material as the only explanation.
