# AGENTS.md

Agent guidance for the **EloqKV** repo (Redis/Valkey-compatible distributed DB;
transaction/storage engine lives in the `data_substrate` submodule).

**Read [docs/README.md](docs/README.md) before unfamiliar architecture work**;
it is the index and authoring standard for the current design. Engine internals
are documented from [data_substrate/docs/README.md](data_substrate/docs/README.md).

## Setup, build, and test

```bash
git submodule update --init --recursive
mkdir -p bld && cd bld
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DWITH_DATA_STORE=ELOQDSS_ROCKSDB -DWITH_LOG_STATE=ROCKSDB \
    -DCMAKE_INSTALL_PREFIX=../install
cmake --build . --parallel "$(nproc)"
cmake --install . --prefix ../install
cd ..
./install/bin/eloqkv --config=eloqkv.ini

# Run one TCL test against the server; loop over tests/unit/eloq/*.tcl for all.
tclsh tests/test_helper.tcl --host 127.0.0.1 --port 6379 \
    --tags -needs:repl --tags -needs:config-maxmemory --tags -needs:debug \
    --tags -needs:redis_config --tags -needs:redis_expire --tags -needs:slow_test \
    --tags -needs:support_cmd_later --tags -needs:cluster_mode \
    --single /unit/eloq/hash
```

Use `redis-cli -h 127.0.0.1 -p 6379` for manual checks. Prefer the
`eloqdata/eloq-dev-ci-ubuntu2404` image; otherwise run
`scripts/install_dependency_ubuntu2404.sh`. Debug builds enable fault injection;
on non-Debug builds exclude `needs:fault_inject` in the TCL runner.

Important CMake switches are `WITH_DATA_STORE` (EloqStore by default; RocksDB is
common locally), `WITH_LOG_STATE`, `WITH_LOG_SERVICE`, and
`BUILD_ELOQKV_AS_LIBRARY`.

## Critical threading boundary

Request handlers run on brpc bthreads, while concurrency-control request
`Execute()` methods run on shard/TxProcessor context. Never share a
`bthread::Mutex` or `bthread::ConditionVariable` between those contexts: it can
permanently deadlock a worker. Completion waits from a bthread must use atomic
state plus `bthread_usleep` backoff. See
[command processing](docs/02-command-processing.md) and the engine's
[threading model](data_substrate/docs/02-threading-model.md).

## Where are the logs?

Server logs are **glog** files that default to **`install/logs/`** — i.e.
`<install_prefix>/logs`, resolved relative to the binary, **not** the current
working directory. You'll find:

- `eloqdb.log.INFO` / `eloqdb.log.WARNING` / `eloqdb.log.ERROR`
- `host_manager.log.*`

The `*.INFO` / `*.WARNING` entries are symlinks pointing at the current
timestamped file (`eloqdb.log.INFO.YYYYMMDD-HHMMSS.<pid>`).

The startup banner reports the resolved directory; `--help` documents only the
override. Use `--log_dir=<dir>` or `--logtostderr` to change the destination.

## Code style

Use C++20 and Google C++ style. Format changed C/C++ files with
`clang-format-18`; naming and project-specific rules are in
[data_substrate/style_guide.md](data_substrate/style_guide.md). Do not add
global or static objects with non-trivial destructors.

## Final delivery and merge policy

A non-trivial coding task is complete only after implementation, verification,
and reviewer-facing documentation are consistent with the final diff.

- Derive summaries and pull request text from the final merge-base diff, not
  memory or only unstaged changes.
- Report the problem, observable behavior, implementation, material design
  decisions, exact verification performed, risks, rollback, and reviewer focus.
- State unrun checks and uncertainty explicitly; never claim a test passed unless
  it was run in the current workspace.
- Squash-merge pull requests. Rewrite the squash title and body to describe the
  complete final diff rather than accepting an individual commit message.
- When a change touches `data_substrate`, review and land that repository's change
  first, then update the submodule pointer to a commit reachable from its target
  branch.
- In Codex, use `$finish-pr` and `$respond-to-review`; in Claude Code, invoke the
  same shared skills as `/finish-pr` and `/respond-to-review`.

<!-- BEGIN bootstrap-project: engineering-standards -->
## Engineering standards

- Explain non-obvious intent, invariants, ownership, failure behavior, compatibility constraints, and performance or safety tradeoffs near the affected code. Do not restate syntax or names.
- Add or update documentation comments for public APIs when the language supports them.
- Correct stale nearby comments while changing behavior.
- Treat architecture updates as claim-driven. For implementation work, update current architecture only when a changed module boundary, core flow, ownership or lifecycle, durable or wire format, external integration, or system-level invariant makes an existing claim or core model false or materially incomplete. Leave it unchanged when no such claim exists. Dedicated documentation work may correct inaccuracies, fill core-design gaps, or consolidate sediment.
- When a change introduces, removes, splits, or merges a durable module, update the architecture taxonomy, the relevant focused documents, and the architecture index (`docs/architecture/README.md` by default, or its existing equivalent) in the same change.
- Keep the overview focused on system context, high-level flows, cross-cutting invariants, and navigation. A repository with at most one durable module may keep readable architecture detail in its overview. When multiple durable modules emerge or detail needs independent navigation, use focused documents and update the architecture index (`docs/architecture/README.md` by default, or its existing equivalent).
- Keep architecture as a compact, present-tense model of current core design and stable rationale. Put change-specific motivation and before/after explanation in the pull request or commit, and keep local algorithms, representation details, and performance mechanics near the affected code; omit those narrower details from architecture rather than cataloging them as non-architectural.
- Revise and consolidate existing architecture prose so it stands on its own without the change that produced it. Follow the authoring standard linked from `docs/README.md`.
- Treat `docs/plans/` and `docs/superpowers/` as historical change context, not authoritative descriptions of the current code.
- Before unfamiliar work, read `docs/README.md` and the relevant architecture documents.
- When code and documentation disagree, treat code as authoritative and repair the documentation in the same change.
<!-- END bootstrap-project: engineering-standards -->
