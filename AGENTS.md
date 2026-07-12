# AGENTS.md

Agent guidance for the **EloqKV** repo (Redis/Valkey-compatible distributed DB;
transaction/storage engine lives in the `data_substrate` submodule).

**Read [CLAUDE.md](CLAUDE.md) first** — it is the source of truth for
architecture, the build/test commands, and code style. This file only restates
the operational essentials plus a few facts that are otherwise hard to discover.

## Build & run (short form; see CLAUDE.md for full flags)

```bash
mkdir -p bld && cd bld
cmake .. -DWITH_DATA_STORE=ELOQDSS_ELOQSTORE -DWITH_LOG_STATE=ROCKSDB \
    -DCMAKE_INSTALL_PREFIX=../install
cmake --build . --parallel "$(nproc)" && cmake --install .
cd ..                                      # install/ and eloqkv.ini are at the repo root
./install/bin/eloqkv --config=eloqkv.ini   # single node, port 6379, foreground
```

## Where are the logs?

Server logs are **glog** files that default to **`install/logs/`** — i.e.
`<install_prefix>/logs`, resolved relative to the binary, **not** the current
working directory. You'll find:

- `eloqdb.log.INFO` / `eloqdb.log.WARNING` / `eloqdb.log.ERROR`
- `host_manager.log.*`

The `*.INFO` / `*.WARNING` entries are symlinks pointing at the current
timestamped file (`eloqdb.log.INFO.YYYYMMDD-HHMMSS.<pid>`).

Discoverability note: nothing states this default except the startup banner
(`Running logs will be written to the following path: …/install/logs`). `--help`
only documents `--log_dir` as the *override*, never the default. To send logs
elsewhere, pass `--log_dir=<dir>` (or `--logtostderr` to log to stderr instead).

## Documentation and delivery

A non-trivial coding task is complete only after implementation, verification,
and reviewer-facing documentation are consistent with the final diff.

### Code comments

- Document non-obvious invariants, concurrency and memory-ordering assumptions,
  ownership/lifetime rules, failure and retry behavior, compatibility constraints,
  and hot-path tradeoffs. Explain **why**, not syntax.
- For transaction or storage changes, call out lock ordering, reader/writer
  visibility, durability boundaries, crash consistency, and idempotency when
  relevant.
- Add documentation comments to new public APIs and externally visible types.
- Do not add comments that merely restate the code. Update stale nearby comments
  when behavior changes.

### Final delivery

- Derive summaries and pull request text from the final merge-base diff, not
  memory or only unstaged changes.
- Report the problem, observable behavior, implementation, material design
  decisions, exact verification performed, risks, rollback, and reviewer focus.
- State unrun checks and uncertainty explicitly; never claim a test passed unless
  it was run in the current workspace.
- When a change touches `data_substrate`, review and land that repository's change
  first, then update the submodule pointer.
- Use `$finish-pr` for completed non-trivial changes and `$respond-to-review` when
  addressing review feedback.
