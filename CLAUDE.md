# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Repo Is

EloqKV is a distributed, Redis/Valkey-compatible database with ACID transactions. This repo contains the **Redis API layer** only; the transaction/storage engine lives in the `data_substrate` submodule (GitHub repo `eloqdata/tx_service`), which has its own CLAUDE.md. Most non-protocol work (concurrency control, transactions, storage, WAL) happens in the submodule.

After cloning: `git submodule update --init --recursive`.

## Technical Docs — Read These First

`docs/` contains module-by-module design documentation (index: `docs/README.md`). **Before working on an unfamiliar module, read its doc**: `01` overview/bootstrap, `02` command processing & transactions, `03` data model (objects/commands/catalog), `04` Lua/pub-sub/blocking commands, `05` namespaces, `06` vector search, `07` RDB-AOF interop & tools. Engine internals are documented in `data_substrate/docs/`.

**Maintenance rule: when a code change alters behavior described in `docs/`, update the corresponding doc in the same change.** Each doc lists the source files it covers.

## Common Commands

```bash
# Configure + build (out-of-source; bld/ and install/ are gitignored output dirs)
mkdir -p bld && cd bld
cmake .. -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DWITH_DATA_STORE=ELOQDSS_ROCKSDB \
    -DWITH_LOG_STATE=ROCKSDB \
    -DCMAKE_INSTALL_PREFIX=../install
cmake --build . --parallel 16
cmake --install . --prefix ../install

# Run a single-node server (port 6379, foreground)
./install/bin/eloqkv --config=eloqkv.ini

# Run a single TCL test file (e.g. tests/unit/eloq/hash.tcl)
# against the running server; loop over tests/unit/eloq/*.tcl for the full suite
tclsh tests/test_helper.tcl --host 127.0.0.1 --port 6379 \
    --tags -needs:repl --tags -needs:config-maxmemory --tags -needs:debug \
    --tags -needs:redis_config --tags -needs:redis_expire --tags -needs:slow_test \
    --tags -needs:support_cmd_later --tags -needs:cluster_mode \
    --single /unit/eloq/hash

# Format changed C/C++ files (clang-format-18; repeat inside data_substrate/)
git diff --name-only --diff-filter=ACMR \
    | grep -E '\.(cpp|cc|cxx|c|hpp|hh|hxx|h)$' | xargs -r clang-format-18 -i
```

Verify behavior manually with `redis-cli -h 127.0.0.1 -p 6379`.

Notes:
- Dependencies: use the `eloqdata/eloq-dev-ci-ubuntu2404` Docker image or run `scripts/install_dependency_ubuntu2404.sh` (ubuntu2404 preferred).
- Debug builds enable fault injection (`WITH_FAULT_INJECT`); add `--tags -needs:fault_inject` to the TCL command on non-Debug builds.
- Key CMake options: `WITH_DATA_STORE` (storage backend: `ELOQDSS_ELOQSTORE` default, `ELOQDSS_ROCKSDB` common for local dev, also DynamoDB/BigTable/RocksDB-Cloud variants), `WITH_LOG_STATE`, `WITH_LOG_SERVICE`, `BUILD_ELOQKV_AS_LIBRARY` (build as library for a converged binary instead of the `eloqkv` executable).

## Architecture

Command flow, top to bottom:

1. **Entry**: `src/redis_server.cpp` (`eloqkv` executable) starts a brpc server with `EloqKV::RedisServiceImpl` (`src/redis_service.cpp`, ~the largest file in the repo) as the Redis-protocol service. Config comes from gflags / an ini file (`eloqkv.ini` template at repo root).
2. **Command parsing**: RESP requests are parsed into command objects (`include/redis_command.h`, `src/redis_command.cpp`). Replies are written through `redis_replier.h` / `output_handler.h`.
3. **Data objects**: each Redis type is a `txservice::TxObject` subclass — `redis_string_object`, `redis_hash_object`, `redis_list_object`, `redis_set_object`, `redis_zset_object` (see `include/redis_object.h` for the base). Commands are applied to these objects inside the tx service, not in the protocol layer.
4. **Into the engine**: `RedisServiceImpl` wraps commands in `txservice` requests (`ObjectCommandTxRequest`, `MultiObjectCommandTxRequest`, defined in `data_substrate/tx_service/include/tx_request.h`) and drives them through a `TransactionExecution` state machine. Keys are `EloqKey` (`include/eloqkv_key.h`); the schema/table mapping is `eloqkv_catalog_factory.h`.
5. **Engine**: the `data_substrate` submodule shards data into CcShards, each owned by a TxProcessor pinned to a core, and handles concurrency control, WAL (log service), checkpointing, and cold-data storage. See `data_substrate/CLAUDE.md` and the module design docs in `data_substrate/docs/`.

Auxiliary subsystems in this layer:
- **Lua/scripting**: `include/lua_interpreter.h`, vendored `lua/`.
- **Pub/Sub**: `include/pub_sub_manager.h`.
- **Namespaces** (multi-tenancy): `include/namespace/`, `src/namespace/`.
- **Vector search**: `include/vector/`, `src/vector/`.
- **Tools**: `src/tools/eloqkv2aof`, `src/tools/eloqkv2rdb` (export to Redis AOF/RDB formats).
- Vendored Redis C sources (`src/redis/`), `crcspeed/`, `fpconv/`.

Threading model to keep in mind: request handlers run on brpc **bthreads** (coroutines on a custom brpc fork), while CC request `Execute()` runs on shard/TxProcessor context. Do not introduce `bthread::Mutex`/`ConditionVariable` shared between shard-side `Execute()` and bthread waiters — this can permanently deadlock a brpc worker. Use `std::atomic` state + `bthread_usleep` backoff polling instead (see data_substrate's CLAUDE.md for details).

## Code Style

Google C++ style, enforced by clang-format-18 (`.clang-format` at repo root). C++20. Naming: functions/classes `MyName`, locals `my_name`, members `my_name_`, enumerators `kEnumName`. Full project conventions: `data_substrate/style_guide.md`.
