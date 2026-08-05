#!/usr/bin/env bash
#
# Bring up the two-node EloqKV cluster the standby paged-object tests need.
#
#   tests/unit_cc/standby_cluster.sh up   [--wal on|off] [--tag NAME] [--hold]
#   tests/unit_cc/standby_cluster.sh down
#
# "up" starts MinIO (EloqStore's cloud backend), a STANDALONE log service, then
# bootstraps and starts a leader and a standby. It prints the two start-script
# paths, which are what the python tests take as arguments:
#
#   python3 tests/unit_cc/standby_paged_apply.py    <start_p.sh> <start_s.sh>
#   python3 tests/unit_cc/standby_failover_paged.py <start_p.sh> <start_s.sh>
#
# Why this exists rather than a handful of ad-hoc commands:
#
#   * The log service must be EXTERNAL. With the built-in one, log group 0
#     lives inside the leader process (local_port + 2), so anything that stops
#     the leader takes the WAL down with it and the standby hangs trying to
#     reach it. `launch_sv` is only built by the log service's own CMakeLists
#     (TEST_LOG_SERVICE), never by the EloqKV build, so this script builds it
#     on first use.
#   * Every clean run needs a FRESH cloud path. Wiping the local data directory
#     while leaving objects under the old EloqStore prefix makes startup fail
#     with "EloqStore start failed with error code: 16", which surfaces later
#     as READ_CATALOG_FAIL. --tag picks the prefix; it defaults to a new one.
#   * State lives under bld-standby/ (gitignored, alongside the build trees)
#     rather than /tmp, which is tmpfs here and does not survive a crash.
#
# Each node runs with 2 cores. Do not raise this and do not build while the
# cluster is up: an 8-job build alongside a running pair has taken WSL down.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${ELOQKV_SB_ROOT:-$REPO/bld-standby}"
ELOQKV_BIN="${ELOQKV_BIN:-$REPO/bld-eloqstore/eloqkv}"
HM_BIN="${HM_BIN:-$REPO/bld-eloqstore/data_substrate/host_manager}"
LOGSV_BLD="${LOGSV_BLD:-$REPO/bld-logsv}"
THIRD_PARTY_LIB="$REPO/data_substrate/third_party/install/lib"
MINIO_DATA="${MINIO_DATA:-$HOME/minio_data}"

PRIMARY_PORT=7401
STANDBY_PORT=7501
LOGSV_PORT=19100
CORES=2

export LD_LIBRARY_PATH="$THIRD_PARTY_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Kill by /proc/<pid>/exe, never by a pkill pattern: a pattern that names the
# process also matches the pkill command line itself.
kill_by_exe() {
    local want="$1" d pid exe
    for d in /proc/[0-9]*; do
        pid="${d#/proc/}"
        exe="$(readlink "$d/exe" 2>/dev/null || true)"
        case "$exe" in
            */"$want") kill -9 "$pid" 2>/dev/null || true ;;
        esac
    done
}

port_open() {
    python3 -c "
import socket,sys
try:
    socket.create_connection(('127.0.0.1',$1),timeout=2).close()
except OSError:
    sys.exit(1)
" 2>/dev/null
}

wait_port() {
    local port="$1" limit="${2:-90}" i=0
    while [ "$i" -lt "$limit" ]; do
        port_open "$port" && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

do_down() {
    for exe in eloqkv host_manager launch_sv; do kill_by_exe "$exe"; done
    sleep 2
    echo "cluster down (MinIO left running)"
}

build_logsv() {
    [ -x "$LOGSV_BLD/launch_sv" ] && return 0
    echo "building launch_sv (one time, several minutes)..."
    mkdir -p "$LOGSV_BLD"
    ( cd "$LOGSV_BLD" && \
      CMAKE_PREFIX_PATH="$REPO/data_substrate/third_party/install" \
      cmake "$REPO/data_substrate/eloq_log_service" \
            -DWITH_LOG_STATE=ROCKSDB -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_CXX_COMPILER=g++-15 -DCMAKE_C_COMPILER=gcc-15 \
            > "$LOGSV_BLD/configure.log" 2>&1 && \
      cmake --build . --target launch_sv --parallel 8 \
            > "$LOGSV_BLD/build.log" 2>&1 )
    [ -x "$LOGSV_BLD/launch_sv" ]
}

write_ini() {
    local file="$1" port="$2" wal="$3" cloud="$4"
    cat > "$file" <<EOF
[local]
ip = 127.0.0.1
port = $port
core_number = $CORES
node_memory_limit_mb = 512
node_log_limit_mb = 128
enable_data_store = true
enable_wal = $wal
txlog_group_replica_num = 1
node_group_replica_num = 1

[cluster]
ip_port_list = 127.0.0.1:$PRIMARY_PORT
standby_ip_port_list = 127.0.0.1:$STANDBY_PORT
txlog_service_list = 127.0.0.1:$LOGSV_PORT

[store]
eloq_store_cloud_store_path = eloqstore/$cloud
eloq_store_cloud_provider = aws
eloq_store_cloud_endpoint = http://127.0.0.1:9000
eloq_store_cloud_region = us-east-1
eloq_store_cloud_access_key = minioadmin
eloq_store_cloud_secret_key = minioadmin
EOF
}

write_start() {
    local file="$1" ini="$2" data="$3" logs="$4"
    cat > "$file" <<EOF
export LD_LIBRARY_PATH=$THIRD_PARTY_LIB
$ELOQKV_BIN --config=$ini --hm_bin_path=$HM_BIN \\
  --eloq_data_path=$data --log_dir=$logs --logbufsecs=0 \\
  --paged_hash_convert_threshold=1 --paged_hash_page_size=4096 \\
  --checkpointer_interval=5 --txlog_group_replica_num=1 --core_number=$CORES
EOF
}

do_up() {
    local wal="off" tag="" hold=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --wal) wal="$2"; shift 2 ;;
            --tag) tag="$2"; shift 2 ;;
            --hold) hold=1; shift ;;
            *) echo "unknown option: $1" >&2; exit 2 ;;
        esac
    done
    # A fresh prefix per run unless the caller names one: reusing a prefix
    # whose local data has been wiped is the error-code-16 trap above.
    [ -n "$tag" ] || tag="run$(date +%m%d%H%M%S)"

    do_down
    build_logsv

    mkdir -p "$ROOT" "$MINIO_DATA"
    if ! port_open 9000; then
        echo "starting MinIO..."
        MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin \
            nohup minio server "$MINIO_DATA" --address 127.0.0.1:9000 \
            > "$ROOT/minio.log" 2>&1 &
        wait_port 9000 60 || { echo "MinIO did not start" >&2; exit 1; }
    fi

    rm -rf "$ROOT/p_data" "$ROOT/s_data" "$ROOT/p_log" "$ROOT/s_log" \
           "$ROOT/bs_log" "$ROOT/logsv"
    mkdir -p "$ROOT/p_log" "$ROOT/s_log" "$ROOT/bs_log" "$ROOT/logsv/glog"

    cat > "$ROOT/start_logsv.sh" <<EOF
export LD_LIBRARY_PATH=$THIRD_PARTY_LIB
$LOGSV_BLD/launch_sv -conf=127.0.0.1:$LOGSV_PORT -node_id=0 \\
  -log_group_replica_num=1 -start_log_group_id=0 \\
  -storage_path=$ROOT/logsv/raft -rocksdb_storage_path=$ROOT/logsv/rocksdb \\
  -log_dir=$ROOT/logsv/glog -logbufsecs=0 -snapshot_interval=600
EOF
    nohup bash "$ROOT/start_logsv.sh" > "$ROOT/logsv_stdout.log" 2>&1 &
    wait_port "$LOGSV_PORT" 90 || { echo "log service did not start" >&2; exit 1; }
    echo "log service listening on $LOGSV_PORT"

    write_ini "$ROOT/primary.ini" "$PRIMARY_PORT" "$wal" "$tag"
    write_ini "$ROOT/standby.ini" "$STANDBY_PORT" "$wal" "$tag"
    write_start "$ROOT/start_p.sh" "$ROOT/primary.ini" "$ROOT/p_data" "$ROOT/p_log"
    write_start "$ROOT/start_s.sh" "$ROOT/standby.ini" "$ROOT/s_data" "$ROOT/s_log"

    echo "bootstrapping (cloud path eloqstore/$tag, wal=$wal)..."
    "$ELOQKV_BIN" --config="$ROOT/primary.ini" --hm_bin_path="$HM_BIN" \
        --eloq_data_path="$ROOT/p_data" --log_dir="$ROOT/bs_log" \
        --logbufsecs=0 --core_number=$CORES --bootstrap \
        > "$ROOT/bootstrap.log" 2>&1
    tail -1 "$ROOT/bootstrap.log"

    nohup bash "$ROOT/start_p.sh" > "$ROOT/p_stdout.log" 2>&1 &
    sleep 3
    nohup bash "$ROOT/start_s.sh" > "$ROOT/s_stdout.log" 2>&1 &
    wait_port "$PRIMARY_PORT" 120 || { echo "node A did not start" >&2; exit 1; }
    wait_port "$STANDBY_PORT" 120 || { echo "node B did not start" >&2; exit 1; }
    # Both ports answer well before the group has a leader; the tests discover
    # roles themselves, but give the election a moment so they do not have to
    # poll from zero.
    sleep 25

    echo
    echo "cluster up. Roles are decided by the ng election, NOT by these paths:"
    echo "  $ROOT/start_p.sh   (port $PRIMARY_PORT)"
    echo "  $ROOT/start_s.sh   (port $STANDBY_PORT)"
    if [ "$hold" -eq 1 ]; then
        # Keep daemonized descendants alive in CI/process sandboxes that reap
        # them as soon as the launcher exits. Omit --hold for normal use.
        trap do_down EXIT INT TERM
        while true; do sleep 30; done
    fi
}

case "${1:-}" in
    up) shift; do_up "$@" ;;
    down) do_down ;;
    *) sed -n '3,16p' "${BASH_SOURCE[0]}"; exit 2 ;;
esac
