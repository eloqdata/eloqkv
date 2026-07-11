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
