#!/bin/bash

# Auto-detect environment in GitHub Actions
if [ -n "${GITHUB_WORKSPACE}" ]; then
  # Set ELOQKV_BASE_PATH if not explicitly provided.
  # The ent CI workflow checks the main repo out to GITHUB_WORKSPACE/eloqkv;
  # fall back to GITHUB_WORKSPACE itself when there is no eloqkv subdir.
  if [ -z "${ELOQKV_BASE_PATH}" ]; then
    if [ -d "${GITHUB_WORKSPACE}/eloqkv" ]; then
      export ELOQKV_BASE_PATH="${GITHUB_WORKSPACE}/eloqkv"
    else
      export ELOQKV_BASE_PATH="${GITHUB_WORKSPACE}"
    fi
  fi
  # Set current_user (used by setup_passwordless_ssh_for_eloq_test)
  if [ -z "${current_user}" ]; then
    current_user=$(whoami)
  fi
  # Path to eloq_test repo (cloned alongside the main repo)
  if [ -z "${ELOQ_TEST_PATH}" ]; then
    export ELOQ_TEST_PATH="${GITHUB_WORKSPACE}/eloq_test_src"
  fi
fi

# Build parallelism: defaults to nproc, caller can override per build type
BUILD_JOBS=${BUILD_JOBS:-$(nproc)}
# Ensure at least 1 to avoid -j 0 (unlimited parallelism)
[ "${BUILD_JOBS}" -lt 1 ] && BUILD_JOBS=1

function kernel_version_greater_than_6.5() {
  kernel_version=$(uname -r)
  major=$(echo "$kernel_version" | cut -d. -f1)
  minor=$(echo "$kernel_version" | cut -d. -f2)

  if ((major > 6)) || { ((major == 6)) && ((minor >= 5)); }; then
    echo "true"
  else
    echo "false"
  fi
}

enable_io_uring=$(kernel_version_greater_than_6.5)

# Function to check if Redis server is ready
function is_redis_ready() {
  redis-cli -h 127.0.0.1 -p 6379 ping | grep -q "PONG"
}

function wait_until_ready() {
  local timeout=300
  local elapsed=0
  local interval=1

  while ! is_redis_ready; do
    sleep $interval
    elapsed=$((elapsed + interval))
    echo "Wait until ready for $elapsed seconds."
    if [ $elapsed -ge $timeout ]; then
      echo "Timeout: Redis is not ready after $timeout seconds."
      return 1
    fi
  done
  echo "Redis is ready."
  return 0
}

# Function to wait until server is finished
function wait_until_finished() {
  local timeout=300
  local elapsed=0
  local interval=1
  local live_pids

  # First, forcefully kill any remaining eloqkv processes.
  # pkill matches by process name, not command line, so it won't
  # accidentally kill the bash scripts that have eloqkv in their path.
  pkill -x eloqkv 2>/dev/null || true
  sleep 2
  pkill -9 -x eloqkv 2>/dev/null || true

  while true; do
    live_pids=$(ps -eo pid=,stat=,comm= | awk '$3 == "eloqkv" && $2 !~ /^Z/ {print $1}')
    if [[ -z "${live_pids}" ]]; then
      break
    fi
    sleep $interval
    elapsed=$((elapsed + interval))
    if [ $elapsed -ge $timeout ]; then
      echo "Timeout: eloqkv process still running after $timeout seconds."
      ps -eo pid,ppid,stat,comm,args | awk '$4 == "eloqkv" {print}'
      return 1
    fi
  done
  if pgrep -x eloqkv > /dev/null 2>&1; then
    echo "Only defunct eloqkv processes remain; continuing."
    ps -eo pid,ppid,stat,comm,args | awk '$4 == "eloqkv" {print}'
  fi
  return 0
}

# Flags every eloqkv launch carries, bootstrap included.
#   eloqkv_base_flags <port> <enable_wal> <enable_data_store>
function eloqkv_base_flags() {
  echo "--port=$1 --core_number=2 --enable_wal=$2 --enable_data_store=$3" \
       "--enable_io_uring=${enable_io_uring}"
}

# Flags fixed for a given store type: bucket coordinates and credentials.
#   eloqkv_store_flags <kv_store_type>
function eloqkv_store_flags() {
  local txlog="--txlog_rocksdb_cloud_bucket_prefix=${ROCKSDB_CLOUD_BUCKET_PREFIX} \
    --txlog_rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME} \
    --txlog_rocksdb_cloud_object_path=${TXLOG_ROCKSDB_CLOUD_OBJECT_PATH} \
    --txlog_rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}"
  local creds="--aws_access_key_id=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID} \
    --aws_secret_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}"

  case "$1" in
    ROCKSDB)
      ;;
    ELOQDSS_ROCKSDB_CLOUD_S3)
      echo "${creds} ${txlog} \
        --rocksdb_cloud_bucket_prefix=${ROCKSDB_CLOUD_BUCKET_PREFIX} \
        --rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME} \
        --rocksdb_cloud_object_path=${ROCKSDB_CLOUD_OBJECT_PATH} \
        --rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}"
      ;;
    ELOQDSS_ELOQSTORE)
      echo "${creds} ${txlog} \
        --eloq_store_cloud_provider=aws \
        --eloq_store_cloud_endpoint=${ROCKSDB_CLOUD_S3_ENDPOINT} \
        --eloq_store_cloud_access_key=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID} \
        --eloq_store_cloud_secret_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY} \
        --eloq_store_cloud_store_path=${ELOQSTORE_BUCKET_NAME}"
      ;;
    *)
      echo "unknown kv_store_type: $1" >&2
      return 1
      ;;
  esac
}

# Start one eloqkv in the background, with its stdout/stderr redirected so a
# crash cannot take down the CI pipeline. Sets ELOQKV_PID.
#   launch_eloqkv <kv_store_type> <log_name> <port> <wal> <data_store> [extra...]
function launch_eloqkv() {
  local store_type=$1 log_name=$2 port=$3 wal=$4 data_store=$5
  shift 5

  echo "redirecting output to /tmp/ to prevent ci pipeline crash"
  # shellcheck disable=SC2046,SC2086
  env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
    ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
    $(eloqkv_base_flags "${port}" "${wal}" "${data_store}") \
    $(eloqkv_store_flags "${store_type}") \
    --maxclients=1000000 \
    --logtostderr=true \
    "$@" \
    >"/tmp/${log_name}.log" 2>&1 &
  ELOQKV_PID=$!
}

# EloqStore tuning and paths shared by every EloqStore launch.
#   eloqstore_flags <eloq_data_path> <eloq_store_data_path>
function eloqstore_flags() {
  echo "--eloq_data_path=$1 --eloq_store_data_path_list=$2" \
       "--node_memory_limit_mb=${NODE_MEMORY_LIMIT_MB:-2048}" \
       "--max_processing_time_microseconds=1000" \
       "--eloq_store_pages_per_file_shift=1" \
       "--eloq_store_reuse_local_files=true"
}

# Single-node scenarios, run for every store type.
#   id | enable_wal | enable_data_store | checkpointer_interval | evicted
# Checkpoint frequency and eviction are varied independently: a small checkpoint
# interval exercises the flush path, kickout_data_for_test exercises the cold
# read path, and pairing them would leave neither covered on its own.
SINGLE_NODE_SCENARIOS=(
  "small_ckpt|true|true|1|false"
  "evicted|true|true|36000|true"
  "no_wal_small_ckpt|false|true|1|false"
  "no_wal_evicted|false|true|10|true"
  "no_wal_no_data_store|false|false|10|false"
)

# Run every entry of SINGLE_NODE_SCENARIOS against one store type.
#   run_single_node_scenarios <kv_store_type> <build_type> [common flags...]
function run_single_node_scenarios() {
  local store_type=$1 build_type=$2
  shift 2

  local entry id wal data_store ckpt evicted extra
  for entry in "${SINGLE_NODE_SCENARIOS[@]}"; do
    IFS='|' read -r id wal data_store ckpt evicted <<< "${entry}"

    extra=()
    if [[ ${evicted} = true ]]; then
      extra+=(--kickout_data_for_test=true)
    fi

    echo "=== single node scenario: ${id} (wal=${wal} data_store=${data_store} ckpt=${ckpt} evicted=${evicted})"
    run_scenario "${store_type}" "redis_server_single_node_${id}" \
      "${build_type}" "${evicted}" "${wal}" "${data_store}" \
      "$@" \
      --checkpointer_interval="${ckpt}" \
      "${extra[@]}"
  done
}

CLUSTER_PORTS=(6379 7379 8379)
CLUSTER_IP_PORT_LIST="127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379"
LOG_SERVICE_IP_PORT="127.0.0.1:9000"

# Start the standalone WAL service the cluster writes to. Sets LOG_SERVICE_PID.
function start_log_service() {
  echo "starting log service"
  rm -rf /tmp/log_data
  ${ELOQKV_BASE_PATH}/install/bin/launch_sv \
    -conf=${LOG_SERVICE_IP_PORT} \
    -node_id=0 \
    -storage_path="/tmp/log_data" \
    --logtostderr=true \
    >/tmp/redis_log_service.log 2>&1 &
  LOG_SERVICE_PID=$!
  echo "log_service is started, pid: $LOG_SERVICE_PID"
  sleep 10
}

# Start the three cluster members. Sets CLUSTER_PIDS.
#   launch_cluster <kv_store_type> <log_prefix> <wal> <data_store> [extra...]
# Per-node flags (data paths, port) are derived from the node index; everything
# else is shared, so callers pass only what the scenario varies.
function launch_cluster() {
  local store_type=$1 log_prefix=$2 wal=$3 data_store=$4
  shift 4

  CLUSTER_PIDS=()
  local index=0 port arg node_args
  for port in "${CLUSTER_PORTS[@]}"; do
    # @INDEX@ in a caller's flag becomes the node index, for per-node paths.
    node_args=()
    for arg in "$@"; do
      node_args+=("${arg//@INDEX@/${index}}")
    done

    launch_eloqkv "${store_type}" "${log_prefix}_${index}" \
      "${port}" "${wal}" "${data_store}" \
      --eloq_data_path="/tmp/redis_server_data_${index}" \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --ip_port_list=${CLUSTER_IP_PORT_LIST} \
      "${node_args[@]}"
    CLUSTER_PIDS+=("${ELOQKV_PID}")
    echo "redis_server ${index} is started, pid: ${ELOQKV_PID}"
    index=$((index + 1))
  done
}

# Stop the cluster members, and the log service if one was started.
function stop_cluster() {
  local pid
  for pid in "${CLUSTER_PIDS[@]}"; do
    if [[ -n $pid && -e /proc/$pid ]]; then
      kill "$pid"
    fi
  done
  if [[ -n ${LOG_SERVICE_PID:-} ]]; then
    kill "${LOG_SERVICE_PID}" 2>/dev/null || true
    LOG_SERVICE_PID=
  fi
  wait_until_finished
}

# Bring up a cluster, run the tcl suite against it, then tear it down.
#   run_cluster_scenario <kv_store_type> <log_prefix> <build_type> <evicted>
#                        <wal> <data_store> [extra...]
function run_cluster_scenario() {
  local store_type=$1 log_prefix=$2 build_type=$3 evicted=$4 wal=$5 data_store=$6
  shift 6

  launch_cluster "${store_type}" "${log_prefix}" "${wal}" "${data_store}" "$@"
  wait_until_ready
  echo "Redis server is ready!"

  run_tcl_tests all "${build_type}" true "${evicted}"

  stop_cluster
}

DSS_SERVER_IP_PORT="127.0.0.1:9100"

# Cluster scenarios, run for every store type.
#   id | enable_wal | enable_data_store | store mode | checkpointer_interval | evicted | log_replay
# "embedded" keeps the data store in the eloqkv process; "dss" puts it behind a
# shared dss_server, which is the deployment the ELOQDSS_* stores exist for.
# ROCKSDB has no dss_server backend, so it skips those entries.
CLUSTER_SCENARIOS=(
  "pure_memory|false|false|embedded|36000|false|false"
  "small_ckpt|true|true|embedded|1|false|true"
  "evicted|true|true|embedded|36000|true|false"
  "no_wal_small_ckpt|false|true|embedded|1|false|false"
  "no_wal_evicted|false|true|embedded|10|true|false"
  "dss_small_ckpt|true|true|dss|1|false|true"
  "dss_evicted|true|true|dss|36000|true|false"
  "dss_no_wal_small_ckpt|false|true|dss|1|false|false"
  "dss_no_wal_evicted|false|true|dss|10|true|false"
)

# Load data into a running cluster, restart it, and check the replayed contents
# match. The cluster must already be up; it is left running on return.
#   run_cluster_log_replay <kv_store_type> <log_prefix> <wal> <data_store> [flags...]
function run_cluster_log_replay() {
  local store_type=$1 log_prefix=$2 wal=$3 data_store=$4
  shift 4

  echo "=== cluster log replay"
  local python_test_file="${ELOQKV_BASE_PATH}/tests/unit/eloq/log_replay_test/log_replay_test.py"
  python3 "$python_test_file" --load > /tmp/load.log 2>&1
  sleep 10

  local pid
  for pid in "${CLUSTER_PIDS[@]}"; do
    if [[ -n $pid && -e /proc/$pid ]]; then
      kill -9 "$pid"
    fi
  done
  wait_until_finished

  launch_cluster "${store_type}" "${log_prefix}_after_replay" "${wal}" "${data_store}" "$@"
  wait_until_ready

  python3 "$python_test_file" --verify
  sleep 10

  if diff <(jq -S . database_snapshot_before_replay.json) \
          <(jq -S . database_snapshot_after_replay.json) &>/dev/null; then
    echo "PASS: The JSON files are identical."
  else
    echo "FAIL: The JSON files are different."
    exit 1
  fi
}

# Run every applicable entry of CLUSTER_SCENARIOS against one store type.
#   run_cluster_scenarios <kv_store_type> <build_type> [common flags...]
function run_cluster_scenarios() {
  local store_type=$1 build_type=$2
  shift 2

  local entry id wal data_store mode ckpt evicted log_replay
  for entry in "${CLUSTER_SCENARIOS[@]}"; do
    IFS='|' read -r id wal data_store mode ckpt evicted log_replay <<< "${entry}"

    if [[ ${mode} = dss && ${store_type} = ROCKSDB ]]; then
      echo "=== cluster scenario ${id}: skipped, ROCKSDB has no dss_server backend"
      continue
    fi

    local extra=(--checkpointer_interval="${ckpt}")
    [[ ${evicted} = true ]] && extra+=(--kickout_data_for_test=true)

    rm -rf /tmp/redis_server_data*

    if [[ ${wal} = true ]]; then
      start_log_service
      extra+=(--txlog_service_list=${LOG_SERVICE_IP_PORT} --txlog_group_replica_num=3)
    fi

    if [[ ${mode} = dss ]]; then
      stop_and_clean_dss_server "${store_type}"
      start_dss_server "127.0.0.1" "9100" "${store_type}"
      extra+=(--eloq_dss_peer_node=${DSS_SERVER_IP_PORT})
    fi

    echo "=== cluster scenario: ${id} (wal=${wal} data_store=${data_store} mode=${mode} ckpt=${ckpt} evicted=${evicted})"

    if [[ ${data_store} = true ]]; then
      bootstrap_eloqkv "${store_type}" \
        --eloq_data_path="/tmp/redis_server_data_0" \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --logtostderr=true \
        --ip_port_list=${CLUSTER_IP_PORT_LIST} \
        "$@" \
        "${extra[@]}"
    fi

    if [[ ${log_replay} = true ]]; then
      launch_cluster "${store_type}" "redis_server_multi_node_${id}" \
        "${wal}" "${data_store}" "$@" "${extra[@]}"
      wait_until_ready
      run_tcl_tests all "${build_type}" true "${evicted}"
      run_cluster_log_replay "${store_type}" "redis_server_multi_node_${id}" \
        "${wal}" "${data_store}" "$@" "${extra[@]}"
      stop_cluster
    else
      run_cluster_scenario "${store_type}" "redis_server_multi_node_${id}" \
        "${build_type}" "${evicted}" "${wal}" "${data_store}" \
        "$@" \
        "${extra[@]}"
    fi

    if [[ ${mode} = dss ]]; then
      stop_and_clean_dss_server "${store_type}"
    fi
  done
}

# Initialise the cluster, then wait for the process to exit on its own.
#   bootstrap_eloqkv <kv_store_type> [extra...]
function bootstrap_eloqkv() {
  local store_type=$1
  shift

  # shellcheck disable=SC2046,SC2086
  env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
    ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
    $(eloqkv_base_flags 6379 true true) \
    $(eloqkv_store_flags "${store_type}") \
    "$@" \
    --bootstrap=true &

  echo "bootstrap is started, pid: $!"
  sleep 20
}

# Launch a single node, run the tcl suite against it, then stop it.
#   run_scenario <kv_store_type> <log_name> <build_type> <evicted> <wal>
#                <data_store> [extra...]
function run_scenario() {
  local store_type=$1 log_name=$2 build_type=$3 evicted=$4 wal=$5 data_store=$6
  shift 6

  launch_eloqkv "${store_type}" "${log_name}" 6379 "${wal}" "${data_store}" "$@"
  wait_until_ready
  echo "Redis server is ready!"

  run_tcl_tests all "${build_type}" false "${evicted}"

  if [[ -n ${ELOQKV_PID} && -e /proc/${ELOQKV_PID} ]]; then
    kill "${ELOQKV_PID}"
  fi
  wait_until_finished
}

function setup_passwordless_ssh_for_eloq_test() {
  local user_home="/home/$current_user"
  local ssh_dir="${user_home}/.ssh"
  local private_key="${ssh_dir}/id_ed25519"
  local public_key="${private_key}.pub"
  local authorized_keys="${ssh_dir}/authorized_keys"
  local sshd_config="/etc/ssh/sshd_config"
  local user_group

  mkdir -p "${ssh_dir}"
  chmod 700 "${ssh_dir}"

  if [ ! -f "${private_key}" ]; then
    ssh-keygen -q -t ed25519 -N "" -f "${private_key}"
  elif [ ! -f "${public_key}" ]; then
    ssh-keygen -y -f "${private_key}" > "${public_key}"
  fi

  touch "${authorized_keys}"
  chmod 600 "${authorized_keys}"
  if ! grep -qxF "$(cat "${public_key}")" "${authorized_keys}"; then
    cat "${public_key}" >> "${authorized_keys}"
    echo >> "${authorized_keys}"
  fi

  user_group=$(id -gn "${current_user}" 2>/dev/null || true)
  if [ -n "${user_group}" ]; then
    chown -R "${current_user}:${user_group}" "${ssh_dir}"
  else
    chown -R "${current_user}" "${ssh_dir}"
  fi

  if [ "$(id -u)" -eq 0 ]; then
    if grep -qE '^[#[:space:]]*MaxStartups[[:space:]]+' "${sshd_config}"; then
      sed -i 's/^[#[:space:]]*MaxStartups[[:space:]].*/MaxStartups 200/' "${sshd_config}"
    else
      printf '\nMaxStartups 200\n' >> "${sshd_config}"
    fi

    if grep -qE '^[#[:space:]]*PubkeyAuthentication[[:space:]]+' "${sshd_config}"; then
      sed -i 's/^[#[:space:]]*PubkeyAuthentication[[:space:]].*/PubkeyAuthentication yes/' "${sshd_config}"
    else
      printf 'PubkeyAuthentication yes\n' >> "${sshd_config}"
    fi
  else
    if sudo -n test -f "${sshd_config}"; then
      if sudo -n grep -qE '^[#[:space:]]*MaxStartups[[:space:]]+' "${sshd_config}"; then
        sudo -n sed -i 's/^[#[:space:]]*MaxStartups[[:space:]].*/MaxStartups 200/' "${sshd_config}"
      else
        printf '\nMaxStartups 200\n' | sudo -n tee -a "${sshd_config}" >/dev/null
      fi

      if sudo -n grep -qE '^[#[:space:]]*PubkeyAuthentication[[:space:]]+' "${sshd_config}"; then
        sudo -n sed -i 's/^[#[:space:]]*PubkeyAuthentication[[:space:]].*/PubkeyAuthentication yes/' "${sshd_config}"
      else
        printf 'PubkeyAuthentication yes\n' | sudo -n tee -a "${sshd_config}" >/dev/null
      fi
    else
      echo "sudo access is required to update ${sshd_config} and restart ssh."
      return 1
    fi
  fi

  if [ "$(id -u)" -eq 0 ]; then
    systemctl restart ssh || service ssh restart
  else
    sudo -n systemctl restart ssh || sudo -n service ssh restart
  fi

  if [ $? -ne 0 ]; then
    echo "Failed to restart ssh service."
    return 1
  fi

  return 0
}

function run_tcl_tests() {
  local test_to_run=$1
  local is_cluster=${3:-false}
  local fault_inject="--tags -needs:fault_inject"
  if [[ $2 = "Debug" ]]; then
    fault_inject=""
  fi
  local evicted=${4:-false}
  local no_evicted="--tags -needs:no_evicted"
  if [[ $evicted = "false" ]]; then
    no_evicted=""
  fi

  local eloqkv_base_path="${ELOQKV_BASE_PATH}"

  cd ${eloqkv_base_path}

  local succeed=true
  local tcl_script_command=" \
    tclsh tests/test_helper.tcl \
    --host 127.0.0.1 \
    --port 6379 \
    --tags -needs:repl \
    --tags -needs:config-maxmemory \
    --tags -needs:debug \
    --tags -needs:redis_config \
    --tags -needs:redis_expire \
    --tags -needs:slow_test \
    --tags -needs:support_cmd_later \
    $fault_inject \
    $no_evicted \
    --single /unit/eloq/"
  local files=$(find ${eloqkv_base_path}/tests/unit/eloq -maxdepth 2 -type f)

  for file in $files; do
    local file_extension="${file##*.}"
    local relative_path="${file#${eloqkv_base_path}/tests/unit/eloq/}"
    relative_path="${relative_path%.*}"

    if [[ "$file_extension" = "tcl" ]]; then
      if [[ "$test_to_run" = "all" || "$relative_path" = "$test_to_run" ]]; then
        echo "Running Tcl script for file: $relative_path"
        local full="$tcl_script_command$relative_path"

        if ! $full; then
          echo "Error running Tcl script for file: $relative_path" >&2
          if [[ "$file" != *"/flaky_test/"* ]]; then
            succeed=false
          else
            echo "The test is flaky, keep running"
          fi
        fi
      fi
    fi

    if [[ $succeed = false ]]; then
      exit 1
    fi
  done
}

function flush_redis_data() {
  local log_file=$1
  shift
  local ports=("$@")

  if [[ ${#ports[@]} -eq 0 ]]; then
    ports=(6379)
  fi

  echo "Flushing Redis data on ports: ${ports[*]}" >>"${log_file}"

  local port
  for port in "${ports[@]}"; do
    if ! redis-cli -h 127.0.0.1 -p "${port}" flushall; then
      echo "Failed to flush Redis data on port ${port}." >&2
      return 1
    fi
  done
}

function cleanup_minio_bucket() {
  bucket_name=$1
  if [[ "$bucket_name" == eloqkv-* ]]; then
    bucket_full_name="${bucket_name}"
  else
    bucket_full_name="eloqkv-${bucket_name}"
  fi
  echo "Clean up bucket ${bucket_full_name}"
  mc rb --force local/${bucket_full_name} 2>/dev/null || true
}

function dump_file_tail() {
  local file=$1
  local lines=${2:-300}

  if [ -f "$file" ]; then
    echo ""
    echo "===== ${file} (last ${lines} lines) ====="
    tail -n "$lines" "$file" || true
  fi
}

function dump_matching_log_lines() {
  local title=$1
  local pattern=$2
  shift 2

  echo ""
  echo "===== ${title} ====="

  local file
  local matched_file=false
  for file in "$@"; do
    if [ ! -f "$file" ]; then
      continue
    fi

    matched_file=true
    echo ""
    echo "----- ${file} -----"
    if command -v rg >/dev/null 2>&1; then
      rg -n -i "$pattern" "$file" | tail -n 250 || true
    else
      grep -E -n -i "$pattern" "$file" | tail -n 250 || true
    fi
  done

  if [ "$matched_file" = false ]; then
    echo "No matching files"
  fi
}

function dump_eloq_test_focused_logs() {
  local runtime_dir=$1

  if [ ! -d "$runtime_dir" ]; then
    return 0
  fi

  echo ""
  echo "===== focused eloq_test rolling-upgrade diagnostics ====="

  echo ""
  echo "===== redis/node configs ====="
  local file
  local found_config=false
  for file in "$runtime_dir"/my_*.cnf; do
    if [ ! -f "$file" ]; then
      continue
    fi

    found_config=true
    echo ""
    echo "----- ${file} -----"
    if command -v rg >/dev/null 2>&1; then
      rg -n '^(eloqkv_port|tx_port|tx_ip|txlog_service_list|txlog_group_replica_num|cluster_config_file|eloq_dss_peer_node|checkpointer_delay_secs)=' "$file" || true
    else
      grep -E -n '^(eloqkv_port|tx_port|tx_ip|txlog_service_list|txlog_group_replica_num|cluster_config_file|eloq_dss_peer_node|checkpointer_delay_secs)=' "$file" || true
    fi
  done
  if [ "$found_config" = false ]; then
    echo "No runtime/my_*.cnf files found"
  fi

  echo ""
  echo "===== cluster configs ====="
  local found_cluster_config=false
  for file in "$runtime_dir"/test_data/cluster_config*.cnf; do
    if [ ! -f "$file" ]; then
      continue
    fi

    found_cluster_config=true
    dump_file_tail "$file" 80
  done
  if [ "$found_cluster_config" = false ]; then
    echo "No runtime/test_data/cluster_config*.cnf files found"
  fi

  dump_file_tail "$runtime_dir/multi_cluster_rolling_upgrade_log" 250

  dump_matching_log_lines \
    "logservice replay diagnostics" \
    'ReplayLog|replay|shipping|log shipping|LogServer|raft|leader|term|ng:|failed|error|fatal' \
    "$runtime_dir"/log_service/node*_log

  dump_matching_log_lines \
    "node recovery and redis startup diagnostics" \
    'Requesting log replay|no recovery connection|ReplayLog|replay service|Failed to ReplayLog|leader|WaitClusterReady|redis|listening|connect|failed|error|fatal|Cluster scale|UpdateClusterConfig' \
    "$runtime_dir"/node*_log

  dump_file_tail "$runtime_dir/log_service/node0_log" 250
  dump_file_tail "$runtime_dir/node5_log" 250
  dump_file_tail "$runtime_dir/node6_log" 250
}

function ensure_gdb_available() {
  if ! command -v gdb >/dev/null 2>&1; then
    echo "gdb not installed; installing..."
    (apt-get update && apt-get install -y gdb) >/dev/null 2>&1 || true
  fi
  command -v gdb >/dev/null 2>&1
}

function dump_live_process_backtraces() {
  # Timeout failures leave the process alive, so core dumps are not enough to
  # diagnose where the server is stuck.
  set +e
  echo ""
  echo "===== live process backtraces ====="

  if ! ensure_gdb_available; then
    echo "gdb unavailable; cannot attach to live processes."
    return 0
  fi

  local pids
  pids=$(ps -eo pid=,comm= | awk '$2 == "eloqkv" || $2 == "dss_server" || $2 == "launch_sv" {print $1}' | head -12)

  if [ -z "$pids" ]; then
    echo "No live eloqkv, dss_server, or launch_sv processes found."
    return 0
  fi

  local pid comm args exe
  for pid in $pids; do
    if ! kill -0 "$pid" 2>/dev/null; then
      continue
    fi

    comm=$(ps -p "$pid" -o comm= 2>/dev/null)
    args=$(ps -p "$pid" -o args= 2>/dev/null)
    exe=$(readlink -f "/proc/${pid}/exe" 2>/dev/null || true)

    echo ""
    echo "----- live backtrace for pid ${pid} (${comm:-unknown}) -----"
    echo "args: ${args:-<unknown>}"
    echo "exe: ${exe:-<unknown>}"

    if command -v timeout >/dev/null 2>&1; then
      timeout 30s gdb -q -batch -nx \
        -p "$pid" \
        -ex 'set pagination off' \
        -ex 'thread apply all bt' \
        -ex 'detach' 2>&1 | head -800 || true
    else
      gdb -q -batch -nx \
        -p "$pid" \
        -ex 'set pagination off' \
        -ex 'thread apply all bt' \
        -ex 'detach' 2>&1 | head -800 || true
    fi
  done
  echo "===== end live process backtraces ====="
}

function dump_core_backtraces() {
  # Print a full backtrace for every core dump we can find. Requires the
  # core_pattern to write a real file (set in gh_ci_entry.sh on the privileged
  # ent-ci runners) and gdb to be installed.
  set +e
  echo ""
  echo "===== core dump backtraces ====="

  if ! ensure_gdb_available; then
    echo "gdb unavailable; cannot symbolize cores. core_pattern=$(cat /proc/sys/kernel/core_pattern 2>/dev/null)"
    return 0
  fi

  # Search the locations we configure core_pattern to use, plus a couple of
  # common fallbacks. Newest first, cap the number we symbolize.
  local cores
  cores=$(find /tmp /var/crash "${ELOQKV_BASE_PATH:-/nonexistent}" -maxdepth 3 -type f \
    -name 'core*' -printf '%T@ %p\n' 2>/dev/null | sort -rn | awk '{print $2}' | head -5)

  if [ -z "$cores" ]; then
    echo "No core files found. core_pattern=$(cat /proc/sys/kernel/core_pattern 2>/dev/null)"
    return 0
  fi

  # Directories that may hold the (symbol-bearing) binaries that produced cores.
  local search_roots="${ELOQKV_BASE_PATH:-.}"

  local core
  for core in $cores; do
    echo ""
    echo "----- backtrace for ${core} -----"

    # The core's %e is the crashing thread's comm name (e.g. brpc_worker:0),
    # not the executable, and the recorded program path is relative
    # (./build/eloqkv), so gdb cannot locate the binary on its own. Read the
    # generating program from the core, then find the real binary and pass it
    # to gdb explicitly so symbols resolve.
    local prog base exe
    prog=$(gdb -q -batch -nx -c "$core" 2>/dev/null \
      | sed -n "s/^Core was generated by \`\\([^ ']*\\).*/\\1/p")
    base=$(basename "${prog:-eloqkv}")
    exe=$(find $search_roots -maxdepth 4 -type f -name "$base" 2>/dev/null | head -1)
    # Fall back to the eloqkv server binary if we couldn't match by name.
    if [ -z "$exe" ]; then
      exe=$(find $search_roots -maxdepth 4 -type f -name eloqkv 2>/dev/null | head -1)
    fi
    echo "program: ${prog:-unknown}  ->  binary: ${exe:-<not found>}"

    if [ -n "$exe" ]; then
      gdb -q -batch -nx \
        -ex 'set pagination off' \
        -ex 'bt full' \
        -ex 'echo \n----- all threads -----\n' \
        -ex 'thread apply all bt' \
        "$exe" "$core" 2>&1 | head -600 || true
    else
      echo "could not locate a binary for ${base}; backtrace without symbols:"
      gdb -q -batch -nx \
        -ex 'set pagination off' \
        -ex 'thread apply all bt' \
        -c "$core" 2>&1 | head -200 || true
    fi
  done
  echo "===== end core dump backtraces ====="
}

function dump_ci_failure_logs() {
  local rc=${1:-1}
  local failed_command=${2:-unknown}

  set +e
  echo ""
  echo "===== CI failure diagnostics ====="
  echo "Exit code: ${rc}"
  echo "Failed command: ${failed_command}"
  date || true
  pwd || true

  echo ""
  echo "===== Running eloq-related processes ====="
  ps -ef | grep -E 'eloqkv|dss_server|launch_sv|minio|redis-server' | grep -v grep || true

  dump_live_process_backtraces
  dump_core_backtraces

  echo ""
  echo "===== eloq_test runtime files ====="
  if [ -n "${ELOQ_TEST_PATH:-}" ] && [ -d "${ELOQ_TEST_PATH}/runtime" ]; then
    dump_eloq_test_focused_logs "${ELOQ_TEST_PATH}/runtime"
    find "${ELOQ_TEST_PATH}/runtime" -maxdepth 3 -type f -printf '%p\n' | sort || true
    while IFS= read -r file; do
      dump_file_tail "$file" 400
    done < <(find "${ELOQ_TEST_PATH}/runtime" -maxdepth 3 -type f \
      \( -name '*log*' -o -name '*.log' -o -name 'LOG' \) | sort)
  else
    echo "No eloq_test runtime directory found at ${ELOQ_TEST_PATH:-<unset>}/runtime"
  fi

  echo ""
  echo "===== /tmp CI logs ====="
  for file in \
    /tmp/minio.log \
    /tmp/eloq_dss_data/eloq_dss_server.log \
    /tmp/redis_single_node.log \
    /tmp/redis_cluster_with_eloqstore.log \
    /tmp/redis_log_service.log \
    /tmp/load.log \
    /tmp/compile_info.log; do
    dump_file_tail "$file" 400
  done

  while IFS= read -r file; do
    dump_file_tail "$file" 250
  done < <(find /tmp -maxdepth 2 -type f \
    \( -name 'redis_server*.log' -o -name 'eloq*.log' -o -name 'dss*.log' \) | sort)

  echo ""
  echo "===== End CI failure diagnostics ====="
}

function prepare_eloqstore_minio_buckets() {
  cleanup_minio_bucket ${ELOQSTORE_BUCKET_NAME}
  cleanup_minio_bucket ${ROCKSDB_CLOUD_BUCKET_NAME}
}

function run_build_ent() {
  local build_type=$1
  local kv_store_type=$2
  local txlog_log_state=$3

  # compile eloqkv
  cd ${ELOQKV_BASE_PATH}
  cmake \
    -S ${ELOQKV_BASE_PATH} \
    -B ${ELOQKV_BASE_PATH}/cmake \
    -DCMAKE_INSTALL_PREFIX=${ELOQKV_BASE_PATH}/install \
    -DCMAKE_BUILD_TYPE=$build_type \
    -DWITH_DATA_STORE=$kv_store_type \
    -DWITH_LOG_STATE=$txlog_log_state \
    -DELOQ_MODULE_ENABLED=ON \
    -DEXT_TX_PROC_ENABLED=ON \
    -DBUILD_WITH_TESTS=ON \
    -DWITH_LOG_SERVICE=ON \
    -DOPEN_LOG_SERVICE=OFF \
    -DFORK_HM_PROCESS=ON

  # Define the output log file
  log_file="/tmp/compile_info.log"

  run_cmake_build() {
    cmake --build ${ELOQKV_BASE_PATH}/cmake -j ${BUILD_JOBS}
    local exit_status=$?

    if [ $exit_status -ne 0 ]; then
      echo "CMake build failed."
      exit $exit_status
    fi
  }

  set +e
  run_cmake_build
  set -e

  # Run the C++ unit tests registered by BUILD_WITH_TESTS=ON
  # (object_serialize_deserialize_test, command_replay_test). They finish in
  # milliseconds and guard replay-determinism paths the TCL suites can't reach.
  ctest --test-dir ${ELOQKV_BASE_PATH}/cmake --output-on-failure

  cmake --install ${ELOQKV_BASE_PATH}/cmake

  # compile log service to setup redis cluster later
  cd ${ELOQKV_BASE_PATH}/data_substrate/eloq_log_service
  cmake -B bld -DCMAKE_BUILD_TYPE=$build_type && cmake --build bld -j ${BUILD_JOBS}
  cp ${ELOQKV_BASE_PATH}/data_substrate/eloq_log_service/bld/launch_sv ${ELOQKV_BASE_PATH}/install/bin/

  case "$kv_store_type" in
  ELOQDSS_*)
    echo "build dss_server"
    cd ${ELOQKV_BASE_PATH}/data_substrate/store_handler/eloq_data_store_service
    cmake -B bld -DCMAKE_BUILD_TYPE=$build_type -DWITH_DATA_STORE=$kv_store_type && cmake --build bld -j ${BUILD_JOBS}
    cp ${ELOQKV_BASE_PATH}/data_substrate/store_handler/eloq_data_store_service/bld/dss_server ${ELOQKV_BASE_PATH}/install/bin/
    ;;
  esac

  cd ${ELOQKV_BASE_PATH}

}

function run_eloqkv_tests() {
  local build_type=$1
  local kv_store_type=$2
  local eloqkv_base_path="${ELOQKV_BASE_PATH}"

  # clean data dir
  rm -rf /tmp/eloq_data

  cd ${eloqkv_base_path}

  if [[ $kv_store_type = "ROCKSDB" ]]; then

    echo "bootstrap rocksdb"

    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --enable_io_uring=${enable_io_uring} \
      --bootstrap=true &

    echo "bootstrap is started, pid: $!"
    # wait for bootstrap to finish
    sleep 20

    # run redis
    launch_eloqkv "$kv_store_type" redis_server_single_node_before_replay \
      6379 true true --checkpointer_interval=36000
    local redis_pid=$ELOQKV_PID

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    run_tcl_tests all $build_type

    # log replay test
    echo "Running log replay test for $build_type build: "

    local python_test_file="${eloqkv_base_path}/tests/unit/eloq/log_replay_test/log_replay_test.py"
    python3 $python_test_file --load > /tmp/load.log 2>&1

    # wait for load to finish
    sleep 10

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill -9 $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    # run redis
    launch_eloqkv "$kv_store_type" redis_server_single_node_after_replay \
      6379 true true --checkpointer_interval=36000
    local redis_pid=$ELOQKV_PID

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    python3 $python_test_file --verify

    # wait for verify to finish
    sleep 10

    file1="database_snapshot_before_replay.json" # First JSON file
    file2="database_snapshot_after_replay.json"  # Second JSON file

    if [[ -z "$file1" || -z "$file2" ]]; then
      echo "ERROR: database_snapshot_before_replay.json or database_snapshot_after_replay.json not generated"
      exit 1
    fi

    # Sort JSON content and compare using diff
    if diff <(jq -S . "$file1") <(jq -S . "$file2") &>/dev/null; then
      echo "PASS: The JSON files are identical."
    else
      echo "FAIL: The JSON files are different."
      exit 1
    fi

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    cd ${ELOQKV_BASE_PATH}
    if [ -d "./cc_ng" ]; then
      rm -rf ./cc_ng
    fi
    if [ -d "./tx_log" ]; then
      rm -rf ./tx_log
    fi
    if [ -d "./eloq_data" ]; then
      rm -rf ./eloq_data
    fi

    # run redis with wal disabled.
    run_single_node_scenarios "$kv_store_type" "$build_type" \
      --node_memory_limit_mb=${NODE_MEMORY_LIMIT_MB:-2048}

  elif [[ $kv_store_type = "ELOQDSS_ROCKSDB_CLOUD_S3" ]]; then

    echo "bootstrap eloqdss-rocksdb-cloud-s3"
    bootstrap_eloqkv "$kv_store_type"

    # run redis
    launch_eloqkv "$kv_store_type" redis_server_single_node_before_replay \
      6379 true true --checkpointer_interval=36000 \
      --rocksdb_cloud_purger_periodicity_secs=30
    local redis_pid=$ELOQKV_PID

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    run_tcl_tests all $build_type

    # log replay test
    echo "Running log replay test for $build_type build: "

    local python_test_file="${eloqkv_base_path}/tests/unit/eloq/log_replay_test/log_replay_test.py"
    python3 $python_test_file --load > /tmp/load.log 2>&1

    # wait for load to finish
    sleep 10

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill -9 $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    # run redis
    launch_eloqkv "$kv_store_type" redis_server_single_node_after_replay \
      6379 true true --checkpointer_interval=36000 \
      --rocksdb_cloud_purger_periodicity_secs=30
    local redis_pid=$ELOQKV_PID

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    python3 $python_test_file --verify

    # wait for verify to finish
    sleep 10

    file1="database_snapshot_before_replay.json" # First JSON file
    file2="database_snapshot_after_replay.json"  # Second JSON file

    if [[ -z "$file1" || -z "$file2" ]]; then
      echo "ERROR: database_snapshot_before_replay.json or database_snapshot_after_replay.json not generated"
      exit 1
    fi

    # Sort JSON content and compare using diff
    if diff <(jq -S . "$file1") <(jq -S . "$file2") &>/dev/null; then
      echo "PASS: The JSON files are identical."
    else
      echo "FAIL: The JSON files are different."
      exit 1
    fi

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    cd ${ELOQKV_BASE_PATH}
    if [ -d "./cc_ng" ]; then
      rm -rf ./cc_ng
    fi
    if [ -d "./tx_log" ]; then
      rm -rf ./tx_log
    fi
    if [ -d "./eloq_data" ]; then
      rm -rf ./eloq_data
    fi

    # run redis with wal disabled.
    run_single_node_scenarios "$kv_store_type" "$build_type" \
      --node_memory_limit_mb=${NODE_MEMORY_LIMIT_MB:-2048} \
      --rocksdb_cloud_purger_periodicity_secs=30

    # clean up bucket in minio
    cleanup_minio_bucket $ROCKSDB_CLOUD_BUCKET_NAME
  elif [[ $kv_store_type = "ELOQDSS_ELOQSTORE" ]]; then
    echo "single eloqkv node with dss_eloqstore." >/tmp/redis_single_node.log

    cleanup_minio_bucket ${ELOQSTORE_BUCKET_NAME}
    cleanup_minio_bucket ${ROCKSDB_CLOUD_BUCKET_NAME}
    local eloq_data_path="/tmp/eloqkv_data"
    local eloq_store_data_path="/tmp/eloqkv_data/eloq_store"
    local store_flags
    store_flags=$(eloqstore_flags "${eloq_data_path}" "${eloq_store_data_path}")

    # run redis
    rm -rf ${eloq_data_path}/*
    echo "big ckpt interval before replay with wal and data store." >>/tmp/redis_single_node.log
    launch_eloqkv "$kv_store_type" redis_server_single_node_before_replay \
      6379 true true ${store_flags} --checkpointer_interval=36000
    local redis_pid=$ELOQKV_PID

    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_single_node.log

    run_tcl_tests all $build_type

    # log replay test
    echo "Running log replay test for $build_type build: "
    local python_test_file="${eloqkv_base_path}/tests/unit/eloq/log_replay_test/log_replay_test.py"
    python3 $python_test_file --load > /tmp/load.log 2>&1
    sleep 10

    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill -9 $redis_pid
    fi
    wait_until_finished

    # run redis
    echo "big ckpt interval after replay with wal and data store." >>/tmp/redis_single_node.log
    launch_eloqkv "$kv_store_type" redis_server_single_node_after_replay \
      6379 true true ${store_flags} --checkpointer_interval=36000
    local redis_pid=$ELOQKV_PID

    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_single_node.log

    python3 $python_test_file --verify
    sleep 10

    file1="database_snapshot_before_replay.json"
    file2="database_snapshot_after_replay.json"
    if diff <(jq -S . "$file1") <(jq -S . "$file2") &>/dev/null; then
      echo "PASS: The JSON files are identical."
    else
      echo "FAIL: The JSON files are different."
      exit 1
    fi

    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi
    wait_until_finished

    cd ${ELOQKV_BASE_PATH}
    rm -rf ./cc_ng ./tx_log ./log_service ./eloq_log_service

    rm -rf ${eloq_data_path}/*
    run_single_node_scenarios "$kv_store_type" "$build_type" ${store_flags}

    cleanup_minio_bucket ${ELOQSTORE_BUCKET_NAME}
    cleanup_minio_bucket ${ROCKSDB_CLOUD_BUCKET_NAME}
  fi

}

# Function to wait until data store server is ready
function wait_dss_until_ready() {
  local dss_server_pid=$1
  local dss_log_path=${2:-/tmp/eloq_dss_data/eloq_dss_server.log}
  local interval=1
  local timeout=300
  local elapsed=0

  while ! grep -qi "DataStoreService Server Started" "${dss_log_path}" 2>/dev/null; do
    if [[ -n "${dss_server_pid}" ]] && ! kill -0 "${dss_server_pid}" 2>/dev/null; then
      echo "Data store server process ${dss_server_pid} exited before becoming ready."
      dump_file_tail "${dss_log_path}" 200
      return 1
    fi
    sleep $interval
    elapsed=$((elapsed + interval))
    echo "Wait until data store server ready for $elapsed seconds."
    if [ $elapsed -ge $timeout ]; then
      echo "Timeout: Data store server is not ready after $elapsed seconds."
      ps -eo pid,ppid,stat,comm,args | awk '$4 == "dss_server" {print}'
      dump_file_tail "${dss_log_path}" 200
      return 1
    fi
  done
  return 0
}

# Function to wait until data store server is finished
function wait_dss_until_finished() {
  local interval=1
  local timeout=120
  local elapsed=0
  local live_pids

  while true; do
    live_pids=$(ps -eo pid=,stat=,comm= | awk '$3 == "dss_server" && $2 !~ /^Z/ {print $1}')
    if [[ -z "${live_pids}" ]]; then
      break
    fi
    sleep $interval
    elapsed=$((elapsed + interval))
    if [ $elapsed -ge $timeout ]; then
      echo "Timeout: Process still running after $timeout seconds."
      # list dss still alived
      ps -eo pid,ppid,stat,comm,args | awk '$4 == "dss_server" {print}'
      return 1
    fi
  done
  if pgrep -x dss_server > /dev/null 2>&1; then
    echo "Only defunct dss_server processes remain; continuing."
    ps -eo pid,ppid,stat,comm,args | awk '$4 == "dss_server" {print}'
  fi
  return 0
}

function stop_and_clean_dss_server() {
  local kv_store_type=$1

  pkill -x dss_server || true
  if ! wait_dss_until_finished; then
    pkill -9 -x dss_server || true
    wait_dss_until_finished
  fi

  rm -f data_store_config.ini
  rm -f /tmp/data_store_config.ini
  rm -rf /tmp/eloq_dss_data

  if [[ $kv_store_type = "ELOQDSS_ROCKSDB_CLOUD_S3" ]]; then
    # clean up bucket in minio
    cleanup_minio_bucket $ROCKSDB_CLOUD_BUCKET_NAME
  fi

}

function start_dss_server() {
  local dss_ip=$1
  local dss_port=$2
  local kv_store_type=$3
  local eloqkv_base_path="${ELOQKV_BASE_PATH}"
  local dss_data_path="/tmp/eloq_dss_data"
  local dss_log_path="/tmp/eloq_dss_data/eloq_dss_server.log"
  local dss_server_configs=

  if [[ $kv_store_type = "ELOQDSS_ROCKSDB_CLOUD_S3" ]]; then
    local rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}
    local rocksdb_cloud_aws_access_key_id=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID}
    local rocksdb_cloud_aws_secret_access_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}
    local rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME}
    local rocksdb_cloud_object_path=${ROCKSDB_CLOUD_OBJECT_PATH}
    dss_server_configs="--rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url} \
                            --aws_access_key_id=${rocksdb_cloud_aws_access_key_id} \
                            --aws_secret_key=${rocksdb_cloud_aws_secret_access_key} \
                            --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
                            --rocksdb_cloud_object_path=${rocksdb_cloud_object_path}"

    elif [[ $kv_store_type = "ELOQDSS_ELOQSTORE" ]]; then
        local eloq_store_worker_num=2
        local eloq_store_data_path="${dss_data_path}/eloq_store"
        local eloq_store_open_files_limit=512
        local eloq_store_cloud_provider=aws
        local eloq_store_cloud_endpoint=${ROCKSDB_CLOUD_S3_ENDPOINT}
        local eloq_store_cloud_access_key=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID}
        local eloq_store_cloud_secret_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}
        local eloq_store_cloud_store_path=${ELOQSTORE_BUCKET_NAME}
        local eloq_store_buffer_pool_size=1MB
        cleanup_minio_bucket ${ELOQSTORE_BUCKET_NAME}
        dss_server_configs="--eloq_store_worker_num=${eloq_store_worker_num} \
                            --eloq_store_data_path_list=${eloq_store_data_path} \
                            --eloq_store_open_files_limit=${eloq_store_open_files_limit} \
                            --eloq_store_cloud_provider=${eloq_store_cloud_provider} \
                            --eloq_store_cloud_endpoint=${eloq_store_cloud_endpoint} \
                            --eloq_store_cloud_access_key=${eloq_store_cloud_access_key} \
                            --eloq_store_cloud_secret_key=${eloq_store_cloud_secret_key} \
                            --eloq_store_cloud_store_path=${eloq_store_cloud_store_path} \
                            --eloq_store_buffer_pool_size=${eloq_store_buffer_pool_size} \
                            --eloq_store_reuse_local_files=true"
    fi

  rm -rf ${dss_data_path}
  mkdir ${dss_data_path}
  echo "starting dss_server"
  ${eloqkv_base_path}/data_substrate/store_handler/eloq_data_store_service/bld/dss_server \
    ${dss_server_configs} \
    --data_path=${dss_data_path} \
    --ip=$dss_ip \
    --port=$dss_port \
    --logtostderr=true \
    >${dss_log_path} 2>&1 \
    &
  local dss_server_pid=$!

  wait_dss_until_ready "$dss_server_pid" "$dss_log_path"
  echo "dss_server is started, pid: $dss_server_pid"
}
function run_eloqkv_cluster_tests() {
  local build_type=$1
  local kv_store_type=$2
  local eloqkv_base_path="${ELOQKV_BASE_PATH}"

  # remove data dir generated by other tests.
  rm -rf /tmp/redis_server_data*

  cd ${eloqkv_base_path}

  # ROCKSDB skips the dss entries; the other two run the full list.
  local store_extra=()
  if [[ $kv_store_type = "ROCKSDB" ]]; then
    store_extra=(--rocksdb_storage_path="/tmp/rocksdb_data_@INDEX@")
  elif [[ $kv_store_type = "ELOQDSS_ELOQSTORE" ]]; then
    store_extra=(--eloq_store_data_path_list="/tmp/redis_server_data_@INDEX@/eloq_store")
  elif [[ $kv_store_type = "ELOQDSS_ROCKSDB_CLOUD_S3" ]]; then
    store_extra=(--rocksdb_cloud_purger_periodicity_secs=30)
  fi

  run_cluster_scenarios "$kv_store_type" "$build_type" \
    --node_memory_limit_mb=${NODE_MEMORY_LIMIT_MB:-2048} \
    "${store_extra[@]}"

  cleanup_minio_bucket ${ROCKSDB_CLOUD_BUCKET_NAME} || true
  cleanup_minio_bucket ${ELOQSTORE_BUCKET_NAME} || true
}

function run_eloq_test() {
  local build_type=$1
  local kv_store_type=$2

  if [[ "$build_type" != "Debug" ]]; then
    echo "Not Debug build type, skip run_eloq_test."
    return 0
  fi

  # Disable grpc fork protection which might blocks forever.
  # https://github.com/grpc/grpc/blob/master/doc/fork_support.md
  export GRPC_ENABLE_FORK_SUPPORT=0

  local eloqkv_install_path="${ELOQKV_BASE_PATH}/install"

  if [ ! -d "${ELOQ_TEST_PATH}/" ]; then
    echo "${ELOQ_TEST_PATH}/ not exists, exit !!!"
  fi

  setup_passwordless_ssh_for_eloq_test

  cd ${ELOQ_TEST_PATH}
  ./setup

  if [ -d "${ELOQ_TEST_PATH}/runtime" ]; then
    rm -rf ${ELOQ_TEST_PATH}/runtime/*
  else
    mkdir ${ELOQ_TEST_PATH}/runtime
  fi

  if [[ $kv_store_type = "ELOQDSS_ROCKSDB_CLOUD_S3" ]]; then

    local rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}
    local rocksdb_cloud_s3_endpoint_url_escape=${ROCKSDB_CLOUD_S3_ENDPOINT_ESCAPE}
    local rocksdb_cloud_aws_access_key_id=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID}
    local rocksdb_cloud_aws_secret_access_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}
    local rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME}
    local rocksdb_cloud_object_path=${ROCKSDB_CLOUD_OBJECT_PATH}
    local txlog_rocksdb_cloud_object_path=${TXLOG_ROCKSDB_CLOUD_OBJECT_PATH}

    echo "rocksdb_cloud_s3_endpoint_url: ${rocksdb_cloud_s3_endpoint_url}"
    sed -i "s/rocksdb_cloud_s3_endpoint_url.*=.\+/rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./storage.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${rocksdb_cloud_aws_access_key_id}/g" ./storage.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./storage.cnf
    sed -i "s/rocksdb_cloud_bucket_name.*=.\+/rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./storage.cnf

    sed -i "s/rocksdb_cloud_s3_endpoint_url.*=.\+/rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/txlog_rocksdb_cloud_s3_endpoint_url.*=.\+/txlog_rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${rocksdb_cloud_aws_access_key_id}/g" ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/rocksdb_cloud_bucket_name.*=.\+/rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/txlog_rocksdb_cloud_bucket_name.*=.\+/txlog_rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/rocksdb_cloud_object_path.*=.\+/rocksdb_cloud_object_path=${rocksdb_cloud_object_path}/g" ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/txlog_rocksdb_cloud_object_path.*=.\+/txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path}/g" ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf

    sed -i "s/rocksdb_cloud_s3_endpoint_url.*=.\+/rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/txlog_rocksdb_cloud_s3_endpoint_url.*=.\+/txlog_rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${rocksdb_cloud_aws_access_key_id}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/rocksdb_cloud_bucket_name.*=.\+/rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/txlog_rocksdb_cloud_bucket_name.*=.\+/txlog_rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/eloqdss_server.cnf

    # run cluster scale tests.
    # python3 redis_test/single_test/cluster_scale_test.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
    # python3 redis_test/single_test/cluster_rolling_upgrade.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
    # python3 redis_test/multi_test/cluster_scale_test.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
    # python3 redis_test/multi_test/cluster_rolling_upgrade.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
    # rm -rf runtime/*

    # run log service scale tests.
    # python3 redis_test/log_service_test/log_service_scale_test.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
    # rm -rf runtime/*

    # TODO: re-enable to run standby tests.
    # python3 run_tests.py --dbtype redis --group standby --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path} --bootstrap true
    # rm -rf runtime/*

    # rm -rf runtime/*
    # python3 redis_test/datastore_test/datastore_scale_test.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
    # python3 run_tests.py --dbtype redis --group single --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}

    # run ttl tests
    rm -rf runtime/*
    # python3 redis_test/ttl_test/ttl_test_with_mem.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
    # python3 redis_test/ttl_test/ttl_test_with_kv.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
    # python3 redis_test/ttl_test/ttl_test_with_wal.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}

    # clean up test bucket
    cleanup_minio_bucket $ROCKSDB_CLOUD_BUCKET_NAME

  elif [[ $kv_store_type = "ELOQDSS_ELOQSTORE" ]]; then
    echo "Run eloq_test for ELOQDSS_ELOQSTORE"
    prepare_eloqstore_minio_buckets
    local rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}
    local rocksdb_cloud_s3_endpoint_url_escape=${ROCKSDB_CLOUD_S3_ENDPOINT_ESCAPE}
    local rocksdb_cloud_aws_access_key_id=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID}
    local rocksdb_cloud_aws_secret_access_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}
    local rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME}
    local rocksdb_cloud_object_path=${ROCKSDB_CLOUD_OBJECT_PATH}
    local txlog_rocksdb_cloud_object_path=${TXLOG_ROCKSDB_CLOUD_OBJECT_PATH}
    local eloqstore_cloud_store_path=${ELOQSTORE_BUCKET_NAME}

    # rocksdb cloud s3 config for txlog
    echo "rocksdb_cloud_s3_endpoint_url: ${rocksdb_cloud_s3_endpoint_url}"
    sed -i "s/rocksdb_cloud_s3_endpoint_url.*=.\+/rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./storage.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${rocksdb_cloud_aws_access_key_id}/g" ./storage.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./storage.cnf
    sed -i "s/rocksdb_cloud_bucket_name.*=.\+/rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./storage.cnf

    sed -i "s/rocksdb_cloud_s3_endpoint_url.*=.\+/rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf
    sed -i "s/txlog_rocksdb_cloud_s3_endpoint_url.*=.\+/txlog_rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${rocksdb_cloud_aws_access_key_id}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf
    sed -i "s/rocksdb_cloud_bucket_name.*=.\+/rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf
    sed -i "s/txlog_rocksdb_cloud_bucket_name.*=.\+/txlog_rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf
    sed -i "s/rocksdb_cloud_object_path.*=.\+/rocksdb_cloud_object_path=${rocksdb_cloud_object_path}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf
    sed -i "s/txlog_rocksdb_cloud_object_path.*=.\+/txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf

    sed -i "s/rocksdb_cloud_s3_endpoint_url.*=.\+/rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/txlog_rocksdb_cloud_s3_endpoint_url.*=.\+/txlog_rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${rocksdb_cloud_aws_access_key_id}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/rocksdb_cloud_bucket_name.*=.\+/rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/txlog_rocksdb_cloud_bucket_name.*=.\+/txlog_rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/rocksdb_cloud_object_path.*=.\+/rocksdb_cloud_object_path=${rocksdb_cloud_object_path}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/txlog_rocksdb_cloud_object_path.*=.\+/txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf

    sed -i "s/rocksdb_cloud_s3_endpoint_url.*=.\+/rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/txlog_rocksdb_cloud_s3_endpoint_url.*=.\+/txlog_rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${rocksdb_cloud_aws_access_key_id}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/rocksdb_cloud_bucket_name.*=.\+/rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/txlog_rocksdb_cloud_bucket_name.*=.\+/txlog_rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g" ./bootstrap_cnf/eloqdss_server.cnf
    echo "rocksdb_cloud_s3_endpoint_url: ${rocksdb_cloud_s3_endpoint_url}"

    # eloqstore config
    sed -i "s/eloq_store_cloud_endpoint.*=.\+/eloq_store_cloud_endpoint=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./storage.cnf
    sed -i "s/eloq_store_cloud_access_key.*=.\+/eloq_store_cloud_access_key=${rocksdb_cloud_aws_access_key_id}/g" ./storage.cnf
    sed -i "s/eloq_store_cloud_secret_key.*=.\+/eloq_store_cloud_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./storage.cnf
    sed -i "s/eloq_store_cloud_store_path.*=.\+/eloq_store_cloud_store_path=${eloqstore_cloud_store_path}/g" ./storage.cnf

    sed -i "s/eloq_store_cloud_endpoint.*=.\+/eloq_store_cloud_endpoint=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf
    sed -i "s/eloq_store_cloud_access_key.*=.\+/eloq_store_cloud_access_key=${rocksdb_cloud_aws_access_key_id}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf
    sed -i "s/eloq_store_cloud_secret_key.*=.\+/eloq_store_cloud_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_local.cnf

    sed -i "s/eloq_store_cloud_endpoint.*=.\+/eloq_store_cloud_endpoint=${rocksdb_cloud_s3_endpoint_url_escape}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/eloq_store_cloud_access_key.*=.\+/eloq_store_cloud_access_key=${rocksdb_cloud_aws_access_key_id}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/eloq_store_cloud_secret_key.*=.\+/eloq_store_cloud_secret_key=${rocksdb_cloud_aws_secret_access_key}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf
    sed -i "s/eloq_store_cloud_store_path.*=.\+/eloq_store_cloud_store_path=${eloqstore_cloud_store_path}/g" ./bootstrap_cnf/*_eloqdss_eloqstore_cloud.cnf

    rm -rf runtime/*
    # TODO(zc) re-enable
    # python3 run_tests.py --dbtype redis --group single --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path}

    # run single/multi test
    rm -rf runtime/*
    # TODO(zc) re-enable
    # prepare_eloqstore_minio_buckets
    # python3 redis_test/multi_test/smoke_test.py --dbtype redis --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path} --bootstrap true

    # TODO(zc) re-enable
    # prepare_eloqstore_minio_buckets
    # python3 redis_test/multi_test/cluster_rolling_upgrade.py --dbtype redis --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path} --bootstrap true
    # TODO(zc) re-enable
    # prepare_eloqstore_minio_buckets
    # python3 redis_test/multi_test/cluster_scale_test.py --dbtype redis --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path} --bootstrap true

    # TODO(zc) re-enable
    # run log service scale test
    # rm -rf runtime/*
    # prepare_eloqstore_minio_buckets
    # python3 redis_test/log_service_test/log_service_scale_test.py --dbtype redis --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path}

    # run standby test
    rm -rf runtime/*
    prepare_eloqstore_minio_buckets
    # TODO: Re-enable after eloqdss-eloqstore-local standby startup no longer
    # times out waiting for standby nodes to become transaction-ready.
    # python3 run_tests.py --dbtype redis --group standby --storage eloqdss-eloqstore-local --install_path ${eloqkv_install_path} --bootstrap true
    rm -rf runtime/*
    # prepare_eloqstore_minio_buckets
    # python3 run_tests.py --dbtype redis --group standby --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path} --bootstrap true
    # rm -rf runtime/*
    # rm -rf runtime/*
    # python3 redis_test/standby_test/test_with_kv.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}
    # sleep 1
    # rm -rf runtime/*
    # python3 redis_test/standby_test/test_with_failover.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}
    # sleep 1
    # rm -rf runtime/*
    # disable unstable test
    #python3 redis_test/standby_test/test_with_wal_and_cass.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}

    # run ttl tests
    # rm -rf runtime/*
    # python3 redis_test/ttl_test/ttl_test_with_mem.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}
    # python3 redis_test/ttl_test/ttl_test_with_kv.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}
    # python3 redis_test/ttl_test/ttl_test_with_wal.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}
  fi

  return 0
}
