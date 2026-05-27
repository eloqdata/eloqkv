#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN_DIR="${ROOT_DIR}/build"
SERVER_BIN="${BIN_DIR}/eloqkv"
CLI_BIN="${ROOT_DIR}/redis-cli"
RUNTIME_DIR="${ROOT_DIR}/tests/tmp/semantic-regressions"
CONF_FILE="${RUNTIME_DIR}/eloqkv.ini"
LOG_FILE="${RUNTIME_DIR}/server.log"

mkdir -p "${RUNTIME_DIR}"
rm -rf "${RUNTIME_DIR}/runtime"

cat >"${CONF_FILE}" <<EOF
[local]
bind_all=true
ip=127.0.0.1
port=6379
eloq_data_path=${RUNTIME_DIR}/runtime
core_number=2
event_dispatcher_num=1
bootstrap=true
enable_data_store=false
enable_wal=off
enable_io_uring=false
auto_redirect=true
maxclients=100000

[cluster]
ip_port_list=127.0.0.1:6379
txlog_group_replica_num=1
EOF

"${SERVER_BIN}" --config="${CONF_FILE}" >"${LOG_FILE}" 2>&1 || true
sed -i 's/bootstrap=true/bootstrap=false/' "${CONF_FILE}"

"${SERVER_BIN}" --config="${CONF_FILE}" >"${LOG_FILE}" 2>&1 &
SERVER_PID=$!
trap 'kill ${SERVER_PID} >/dev/null 2>&1 || true; wait ${SERVER_PID} >/dev/null 2>&1 || true' EXIT

for _ in $(seq 1 30); do
    if "${CLI_BIN}" -p 6379 ping >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

"${CLI_BIN}" -p 6379 ping | grep -qx 'PONG'

"${CLI_BIN}" -p 6379 flushall >/dev/null

[[ "$("${CLI_BIN}" -p 6379 srem missing-set member)" == "0" ]]

"${CLI_BIN}" -p 6379 set dst-string value >/dev/null
[[ "$("${CLI_BIN}" -p 6379 smove missing-src dst-string member)" == "0" ]]
[[ "$("${CLI_BIN}" -p 6379 get dst-string)" == "value" ]]

"${CLI_BIN}" -p 6379 del src-set dst-set >/dev/null
"${CLI_BIN}" -p 6379 sadd src-set member >/dev/null
"${CLI_BIN}" -p 6379 sadd dst-set member >/dev/null
[[ "$("${CLI_BIN}" -p 6379 smove src-set dst-set member)" == "1" ]]
[[ -z "$("${CLI_BIN}" -p 6379 smembers src-set)" ]]
[[ "$("${CLI_BIN}" -p 6379 smembers dst-set)" == "member" ]]
