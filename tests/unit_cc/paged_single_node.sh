#!/usr/bin/env bash
# Reproducible single-node EloqStore harness for paged-object lanes S0/S1.
#
#   paged_single_node.sh up --wal off|on [--tag NAME] [--hold]
#   paged_single_node.sh down
#
# Each run gets a fresh local directory and EloqStore cloud prefix. WAL=on
# starts the same external log service used by standby_cluster.sh; merely
# setting enable_wal without that service is not accepted as an S1 test.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="${ELOQKV_PAGED_SINGLE_ROOT:-$REPO/bld-paged-matrix/single}"
ELOQKV_BIN="${ELOQKV_BIN:-$REPO/bld-eloqstore/eloqkv}"
HM_BIN="${HM_BIN:-$REPO/bld-eloqstore/data_substrate/host_manager}"
LOGSV_BIN="${LOGSV_BIN:-$REPO/bld-logsv/launch_sv}"
THIRD_PARTY_LIB="$REPO/data_substrate/third_party/install/lib"
MINIO_DATA="${MINIO_DATA:-$ROOT/minio-data}"
PORT="${ELOQKV_PAGED_SINGLE_PORT:-7399}"
LOGSV_PORT="${ELOQKV_PAGED_LOGSV_PORT:-19100}"
# Paged knobs, overridable per lane. The default (threshold 1, 4 KB pages)
# puts every stock behavior on the paged representation; the MID-THRESHOLD
# lane used by paged_ttl_convert_txn.py needs threshold=4096 page=131072 so
# a small hash stays monolithic until a transaction grows it.
PAGED_THRESHOLD="${ELOQKV_PAGED_THRESHOLD:-1}"
PAGED_PAGE_SIZE="${ELOQKV_PAGED_PAGE_SIZE:-4096}"
NODE_MEMORY_MB="${ELOQKV_PAGED_NODE_MEMORY_MB:-384}"

export LD_LIBRARY_PATH="$THIRD_PARTY_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

port_open() {
    python3 -c "import socket; s=socket.create_connection(('127.0.0.1',$1),2); s.close()" \
        >/dev/null 2>&1
}

wait_port() {
    local port="$1" limit="${2:-120}" i=0
    while [ "$i" -lt "$limit" ]; do
        port_open "$port" && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

kill_marked_processes() {
    local marker="$1" d pid argv
    for d in /proc/[0-9]*; do
        pid="${d#/proc/}"
        [ -r "$d/cmdline" ] || continue
        argv="$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null || true)"
        case "$argv" in
            *"$marker"*) kill "$pid" 2>/dev/null || true ;;
        esac
    done
}

do_down() {
    if [ -f "$ROOT/current.env" ]; then
        # shellcheck disable=SC1090
        source "$ROOT/current.env"
        kill_marked_processes "$RUN_DIR"
    fi
    sleep 2
    echo "single-node harness down"
}

do_up() {
    local wal="" tag="" hold=0
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --wal) wal="$2"; shift 2 ;;
            --tag) tag="$2"; shift 2 ;;
            --hold) hold=1; shift ;;
            *) echo "unknown option: $1" >&2; exit 2 ;;
        esac
    done
    case "$wal" in on|off) ;; *) echo "--wal must be on or off" >&2; exit 2 ;; esac
    [ -x "$ELOQKV_BIN" ] || { echo "missing EloqStore eloqkv: $ELOQKV_BIN" >&2; exit 1; }
    [ -x "$HM_BIN" ] || { echo "missing host_manager: $HM_BIN" >&2; exit 1; }
    if [ "$wal" = on ] && [ ! -x "$LOGSV_BIN" ]; then
        echo "missing external log service: $LOGSV_BIN" >&2
        echo "build data_substrate/eloq_log_service with TEST_LOG_SERVICE first" >&2
        exit 1
    fi

    mkdir -p "$ROOT" "$MINIO_DATA"
    do_down
    [ -n "$tag" ] || tag="run$(date +%Y%m%d-%H%M%S)-$$"
    local run_dir="$ROOT/$wal-$tag"
    mkdir -p "$run_dir/logs" "$run_dir/bootstrap-logs" "$run_dir/logsv-logs"

    if ! port_open 9000; then
        MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin \
            nohup minio server "$MINIO_DATA" --address 127.0.0.1:9000 \
            > "$ROOT/minio.log" 2>&1 &
        wait_port 9000 60 || { echo "MinIO did not start" >&2; exit 1; }
    fi

    if [ "$wal" = on ]; then
        cat > "$run_dir/start_logsv.sh" <<EOF
export LD_LIBRARY_PATH=$THIRD_PARTY_LIB
$LOGSV_BIN -conf=127.0.0.1:$LOGSV_PORT -node_id=0 \\
  -log_group_replica_num=1 -start_log_group_id=0 \\
  -storage_path=$run_dir/logsv-raft -rocksdb_storage_path=$run_dir/logsv-rocksdb \\
  -log_dir=$run_dir/logsv-logs -logbufsecs=0 -snapshot_interval=600
EOF
        nohup bash "$run_dir/start_logsv.sh" > "$run_dir/logsv.stdout" 2>&1 &
        wait_port "$LOGSV_PORT" 90 || { echo "log service did not start" >&2; exit 1; }
    fi

    cat > "$run_dir/eloqkv.ini" <<EOF
[local]
ip = 127.0.0.1
port = $PORT
core_number = 2
node_memory_limit_mb = $NODE_MEMORY_MB
node_log_limit_mb = 128
enable_data_store = true
enable_wal = $wal
txlog_group_replica_num = 1
node_group_replica_num = 1

[cluster]
ip_port_list = 127.0.0.1:$PORT
$(if [ "$wal" = on ]; then echo "txlog_service_list = 127.0.0.1:$LOGSV_PORT"; fi)

[store]
eloq_store_cloud_store_path = eloqstore/paged-single-$tag
eloq_store_cloud_provider = aws
eloq_store_cloud_endpoint = http://127.0.0.1:9000
eloq_store_cloud_region = us-east-1
eloq_store_cloud_access_key = minioadmin
eloq_store_cloud_secret_key = minioadmin
EOF

    cat > "$run_dir/start.sh" <<EOF
export LD_LIBRARY_PATH=$THIRD_PARTY_LIB
$ELOQKV_BIN --config=$run_dir/eloqkv.ini --hm_bin_path=$HM_BIN \\
  --eloq_data_path=$run_dir/data --log_dir=$run_dir/logs --logbufsecs=0 \\
  --paged_hash_convert_threshold=$PAGED_THRESHOLD --paged_hash_page_size=$PAGED_PAGE_SIZE \\
  --checkpointer_interval=5 --txlog_group_replica_num=1 --core_number=2
EOF

    # Same durable namespace and local state, but creation of new paged
    # objects is dark. paged_dark_feature.py restarts through this command to
    # prove that old paged rows remain readable while new conversions stop.
    cat > "$run_dir/start-dark.sh" <<EOF
export LD_LIBRARY_PATH=$THIRD_PARTY_LIB
$ELOQKV_BIN --config=$run_dir/eloqkv.ini --hm_bin_path=$HM_BIN \\
  --eloq_data_path=$run_dir/data --log_dir=$run_dir/logs --logbufsecs=0 \\
  --paged_hash_convert_threshold=0 --paged_hash_page_size=$PAGED_PAGE_SIZE \\
  --checkpointer_interval=5 --txlog_group_replica_num=1 --core_number=2
EOF

    "$ELOQKV_BIN" --config="$run_dir/eloqkv.ini" --hm_bin_path="$HM_BIN" \
        --eloq_data_path="$run_dir/data" --log_dir="$run_dir/bootstrap-logs" \
        --logbufsecs=0 --core_number=2 --bootstrap \
        > "$run_dir/bootstrap.stdout" 2>&1
    nohup bash "$run_dir/start.sh" > "$run_dir/eloqkv.stdout" 2>&1 &
    wait_port "$PORT" 120 || { echo "EloqKV did not start" >&2; exit 1; }
    sleep 8

    cat > "$ROOT/current.env" <<EOF
RUN_DIR=$run_dir
START_SCRIPT=$run_dir/start.sh
START_DARK_SCRIPT=$run_dir/start-dark.sh
LOG_DIR=$run_dir/logs
PORT=$PORT
WAL=$wal
PAGED_THRESHOLD=$PAGED_THRESHOLD
PAGED_PAGE_SIZE=$PAGED_PAGE_SIZE
NODE_MEMORY_MB=$NODE_MEMORY_MB
EOF
    echo "single-node EloqStore up: wal=$wal port=$PORT"
    echo "  run:   $run_dir"
    echo "  start: $run_dir/start.sh"
    echo "  logs:  $run_dir/logs"
    if [ "$hold" -eq 1 ]; then
        # CI/process sandboxes commonly reap daemonized descendants when the
        # launcher exits. Keep the launcher alive there; ordinary developers
        # can omit --hold and retain the convenient background behavior.
        trap do_down EXIT INT TERM
        while true; do sleep 30; done
    fi
}

case "${1:-}" in
    up) shift; do_up "$@" ;;
    down) do_down ;;
    *) sed -n '2,6p' "${BASH_SOURCE[0]}"; exit 2 ;;
esac
