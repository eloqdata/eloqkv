#!/bin/bash

# Auto-detect environment in GitHub Actions
if [ -n "${GITHUB_WORKSPACE}" ]; then
  # Set ELOQKV_BASE_PATH if not explicitly provided.
  # For single-repo workflows (smoke), the checkout is directly at GITHUB_WORKSPACE.
  # For multi-repo workflows (ent CI), the main repo is checked out to GITHUB_WORKSPACE/eloqkv.
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

  # First, forcefully kill any remaining eloqkv processes.
  # pkill matches by process name, not command line, so it won't
  # accidentally kill the bash scripts that have eloqkv in their path.
  pkill eloqkv 2>/dev/null || true
  sleep 2
  pkill -9 eloqkv 2>/dev/null || true

  while pgrep eloqkv > /dev/null 2>&1; do
    sleep $interval
    elapsed=$((elapsed + interval))
    if [ $elapsed -ge $timeout ]; then
      echo "Timeout: eloqkv process still running after $timeout seconds."
      ps aux | grep eloqkv | grep -v grep | grep -v launch_sv | grep -v dss_server
      return 1
    fi
  done
  return 0
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

function create_minio_bucket()
{
  bucket_name=$1
  if [[ "$bucket_name" == eloqkv-* ]]; then
    bucket_full_name="${bucket_name}"
  else
    bucket_full_name="eloqkv-${bucket_name}"
  fi
  echo "Create bucket ${bucket_full_name}"
  mc mb local/${bucket_full_name} 2>/dev/null || true
  echo "Minio bucket ${bucket_full_name} has been created."
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

  echo ""
  echo "===== eloq_test runtime files ====="
  if [ -n "${ELOQ_TEST_PATH:-}" ] && [ -d "${ELOQ_TEST_PATH}/runtime" ]; then
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
  create_minio_bucket ${ELOQSTORE_BUCKET_NAME}
  create_minio_bucket ${ROCKSDB_CLOUD_BUCKET_NAME}
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
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --enable_io_uring=${enable_io_uring} \
      --logtostderr=true \
      >/tmp/redis_server_single_node_before_replay.log 2>&1 \
      &

    local redis_pid=$!

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
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --enable_io_uring=${enable_io_uring} \
      --logtostderr=true \
      >/tmp/redis_server_single_node_after_replay.log 2>&1 \
      &

    local redis_pid=$!

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
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=true \
      --maxclients=1000000 \
      --checkpointer_interval=10 \
      --enable_io_uring=${enable_io_uring} \
      --logtostderr=true \
      >/tmp/redis_server_single_node_no_wal.log 2>&1 \
      &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    run_tcl_tests all $build_type

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    # run redis with wal and data store disabled.
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=false \
      --maxclients=1000000 \
      --checkpointer_interval=10 \
      --enable_io_uring=${enable_io_uring} \
      --logtostderr=true \
      >/tmp/redis_server_single_node_no_wal_no_data_store.log 2>&1 \
      &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    run_tcl_tests all $build_type

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

  elif [[ $kv_store_type = "ELOQDSS_ROCKSDB_CLOUD_S3" ]]; then

    echo "bootstrap eloqdss-rocksdb-cloud-s3"

    local rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}
    local rocksdb_cloud_aws_access_key_id=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID}
    local rocksdb_cloud_aws_secret_access_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}
    local rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME}
    local rocksdb_cloud_object_path=${ROCKSDB_CLOUD_OBJECT_PATH}
    local rocksdb_cloud_bucket_prefix=${ROCKSDB_CLOUD_BUCKET_PREFIX}
    local txlog_rocksdb_cloud_bucket_prefix=${ROCKSDB_CLOUD_BUCKET_PREFIX}
    local txlog_rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME}
    local txlog_rocksdb_cloud_object_path=${TXLOG_ROCKSDB_CLOUD_OBJECT_PATH}
    local txlog_rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}

    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
      --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
      --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --enable_io_uring=${enable_io_uring} \
      --bootstrap=true &

    echo "bootstrap is started, pid: $!"
    # wait for bootstrap to finish
    sleep 20

    # run redis
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
      --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
      --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --rocksdb_cloud_purger_periodicity_secs=30 \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --enable_io_uring=${enable_io_uring} \
      --logtostderr=true \
      >/tmp/redis_server_single_node_before_replay.log 2>&1 \
      &

    local redis_pid=$!

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
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
      --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
      --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --rocksdb_cloud_purger_periodicity_secs=30 \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --enable_io_uring=${enable_io_uring} \
      --logtostderr=true \
      >/tmp/redis_server_single_node_after_replay.log 2>&1 \
      &

    local redis_pid=$!

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
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=true \
      --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
      --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
      --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --rocksdb_cloud_purger_periodicity_secs=30 \
      --maxclients=1000000 \
      --checkpointer_interval=10 \
      --enable_io_uring=${enable_io_uring} \
      --logtostderr=true \
      >/tmp/redis_server_single_node_no_wal.log 2>&1 \
      &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    run_tcl_tests all $build_type

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    # run redis with wal and data store disabled.
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=false \
      --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
      --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
      --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --rocksdb_cloud_purger_periodicity_secs=30 \
      --maxclients=1000000 \
      --checkpointer_interval=10 \
      --enable_io_uring=${enable_io_uring} \
      --logtostderr=true \
      >/tmp/redis_server_single_node_no_wal_no_data_store.log 2>&1 \
      &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    run_tcl_tests all $build_type

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    # clean up bucket in minio
    cleanup_minio_bucket $ROCKSDB_CLOUD_BUCKET_NAME
  elif [[ $kv_store_type = "ELOQDSS_ELOQSTORE" ]]; then
    echo "single eloqkv node with dss_eloqstore." >/tmp/redis_single_node.log

    cleanup_minio_bucket ${ELOQSTORE_BUCKET_NAME}
    cleanup_minio_bucket ${ROCKSDB_CLOUD_BUCKET_NAME}
    create_minio_bucket ${ELOQSTORE_BUCKET_NAME}
    local eloq_data_path="/tmp/eloqkv_data"
    local node_memory_limit_mb=${NODE_MEMORY_LIMIT_MB:-2048}
    local eloq_store_data_path="/tmp/eloqkv_data/eloq_store"
    local eloqkv_bin_path="${ELOQKV_BASE_PATH}/install/bin/eloqkv"
    local aws_access_key_id=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID}
    local aws_secret_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}
    local txlog_rocksdb_cloud_bucket_prefix=${ROCKSDB_CLOUD_BUCKET_PREFIX}
    local txlog_rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME}
    local txlog_rocksdb_cloud_object_path=${TXLOG_ROCKSDB_CLOUD_OBJECT_PATH}
    local txlog_rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}
    local eloq_store_cloud_provider=aws
    local eloq_store_cloud_store_path=${ELOQSTORE_BUCKET_NAME}
    local eloq_store_cloud_endpoint=${ROCKSDB_CLOUD_S3_ENDPOINT}
    local eloq_store_cloud_access_key=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID}
    local eloq_store_cloud_secret_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}

    # run redis with small ckpt interval.
    rm -rf ${eloq_data_path}/*
    echo "redirecting output to /tmp/ to prevent ci pipeline crash" >>/tmp/redis_single_node.log
    echo "small ckpt interval with wal and data store." >>/tmp/redis_single_node.log
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
    ${eloqkv_bin_path} \
        --port=6379 \
        --core_number=2 \
        --enable_wal=true \
        --enable_data_store=true \
        --node_memory_limit_mb=${node_memory_limit_mb} \
        --eloq_data_path=${eloq_data_path} \
        --eloq_store_data_path_list=${eloq_store_data_path} \
        --maxclients=1000000 \
	      --logtostderr=true \
        --checkpointer_interval=1 \
	      --kickout_data_for_test=true \
        --aws_access_key_id=${aws_access_key_id} \
        --aws_secret_key=${aws_secret_key} \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url=${txlog_rocksdb_cloud_s3_endpoint_url} \
	      --max_processing_time_microseconds=1000 \
	      --eloq_store_pages_per_file_shift=1 \
        --eloq_store_cloud_provider=${eloq_store_cloud_provider} \
        --eloq_store_cloud_endpoint=${eloq_store_cloud_endpoint} \
        --eloq_store_cloud_access_key=${eloq_store_cloud_access_key} \
        --eloq_store_cloud_secret_key=${eloq_store_cloud_secret_key} \
        --eloq_store_cloud_store_path=${eloq_store_cloud_store_path} \
        --eloq_store_reuse_local_files=true \
        >/tmp/redis_server_single_node_small_ckpt_interval.log 2>&1 \
        &

    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_single_node.log

    run_tcl_tests all $build_type false true

    echo "finished small ckpt interval with wal and data store." >>/tmp/redis_single_node.log
    echo "" >>/tmp/redis_single_node.log
    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    # run redis
    rm -rf ${eloq_data_path}/*
    echo "redirecting output to /tmp/ to prevent ci pipeline crash" >>/tmp/redis_single_node.log
    echo "big ckpt interval before replay with wal and data store." >>/tmp/redis_single_node.log
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${eloqkv_bin_path} \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --node_memory_limit_mb=${node_memory_limit_mb} \
      --eloq_data_path=${eloq_data_path} \
      --eloq_store_data_path_list=${eloq_store_data_path} \
      --maxclients=1000000 \
      --logtostderr=true \
      --checkpointer_interval=36000 \
      --aws_access_key_id=${aws_access_key_id} \
      --aws_secret_key=${aws_secret_key} \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url=${txlog_rocksdb_cloud_s3_endpoint_url} \
	    --max_processing_time_microseconds=1000 \
	    --eloq_store_pages_per_file_shift=1 \
      --eloq_store_cloud_provider=${eloq_store_cloud_provider} \
      --eloq_store_cloud_endpoint=${eloq_store_cloud_endpoint} \
      --eloq_store_cloud_access_key=${eloq_store_cloud_access_key} \
      --eloq_store_cloud_secret_key=${eloq_store_cloud_secret_key} \
      --eloq_store_cloud_store_path=${eloq_store_cloud_store_path} \
      --eloq_store_reuse_local_files=true \
      >/tmp/redis_server_single_node_before_replay.log 2>&1 \
      &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_single_node.log

    run_tcl_tests all $build_type
    echo "finished big ckpt interval before replay with wal and data store." >>/tmp/redis_single_node.log
    echo "" >>/tmp/redis_single_node.log

    # log replay test
    echo "Running log replay test for $build_type build: " >>/tmp/redis_single_node.log
    echo "" >>/tmp/redis_single_node.log

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
    echo "redirecting output to /tmp/ to prevent ci pipeline crash" >>/tmp/redis_single_node.log
    echo "big ckpt interval after replay with wal and data store." >>/tmp/redis_single_node.log
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${eloqkv_bin_path} \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --node_memory_limit_mb=${node_memory_limit_mb} \
      --eloq_data_path=${eloq_data_path} \
      --eloq_store_data_path_list=${eloq_store_data_path} \
      --maxclients=1000000 \
      --logtostderr=true \
      --checkpointer_interval=36000 \
      --aws_access_key_id=${aws_access_key_id} \
      --aws_secret_key=${aws_secret_key} \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url=${txlog_rocksdb_cloud_s3_endpoint_url} \
	    --max_processing_time_microseconds=1000 \
	    --eloq_store_pages_per_file_shift=1 \
      --eloq_store_cloud_provider=${eloq_store_cloud_provider} \
      --eloq_store_cloud_endpoint=${eloq_store_cloud_endpoint} \
      --eloq_store_cloud_access_key=${eloq_store_cloud_access_key} \
      --eloq_store_cloud_secret_key=${eloq_store_cloud_secret_key} \
      --eloq_store_cloud_store_path=${eloq_store_cloud_store_path} \
      --eloq_store_reuse_local_files=true \
      >/tmp/redis_server_single_node_after_replay.log 2>&1 \
      &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_single_node.log

    echo "verify log replay result" >>/tmp/redis_single_node.log
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

    echo "finished big ckpt interval after replay with wal and data store." >>/tmp/redis_single_node.log
    echo "" >>/tmp/redis_single_node.log
    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    cd ${eloq_data_path}
    if [ -d "./cc_ng" ]; then
      rm -rf ./cc_ng
    fi
    if [ -d "./tx_log" ]; then
      rm -rf ./tx_log
    fi
    if [ -d "./log_service" ]; then
      rm -rf ./log_service
    fi
    if [ -d "./eloq_log_service" ]; then
      rm -rf ./eloq_log_service
    fi

    # run redis with wal disabled.
    rm -rf ${eloq_data_path}/*
    echo "redirecting output to /tmp/ to prevent ci pipeline crash" >> /tmp/redis_single_node.log
    echo "default ckpt interval without wal and with data store." >> /tmp/redis_single_node.log
    ${eloqkv_bin_path} \
        --port=6379 \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
	      --node_memory_limit_mb=${node_memory_limit_mb} \
        --eloq_data_path=${eloq_data_path} \
        --eloq_store_data_path_list=${eloq_store_data_path} \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=10 \
        --aws_access_key_id=${aws_access_key_id} \
        --aws_secret_key=${aws_secret_key} \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url=${txlog_rocksdb_cloud_s3_endpoint_url} \
	      --max_processing_time_microseconds=1000 \
	      --eloq_store_pages_per_file_shift=1 \
        --eloq_store_cloud_provider=${eloq_store_cloud_provider} \
        --eloq_store_cloud_endpoint=${eloq_store_cloud_endpoint} \
        --eloq_store_cloud_access_key=${eloq_store_cloud_access_key} \
        --eloq_store_cloud_secret_key=${eloq_store_cloud_secret_key} \
        --eloq_store_cloud_store_path=${eloq_store_cloud_store_path} \
        --eloq_store_reuse_local_files=true \
        >/tmp/redis_server_single_node_no_wal_default_ckpt_interval.log 2>&1 \
        &

    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_single_node.log

    run_tcl_tests all $build_type

    echo "finished default ckpt interval without wal and with data store." >>/tmp/redis_single_node.log
    echo "" >>/tmp/redis_single_node.log
    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    # run redis with wal disabled and small ckpt interval.
    rm -rf ${eloq_data_path}/*

    echo "redirecting output to /tmp/ to prevent ci pipeline crash" >> /tmp/redis_single_node.log
    echo "samll ckpt interval without wal and with data store." >> /tmp/redis_single_node.log
    ${eloqkv_bin_path} \
        --port=6379 \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
	      --node_memory_limit_mb=${node_memory_limit_mb} \
        --eloq_data_path=${eloq_data_path} \
        --eloq_store_data_path_list=${eloq_store_data_path} \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=1 \
	      --kickout_data_for_test=true \
        --aws_access_key_id=${aws_access_key_id} \
        --aws_secret_key=${aws_secret_key} \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url=${txlog_rocksdb_cloud_s3_endpoint_url} \
	      --max_processing_time_microseconds=1000 \
	      --eloq_store_pages_per_file_shift=1 \
        --eloq_store_cloud_provider=${eloq_store_cloud_provider} \
        --eloq_store_cloud_endpoint=${eloq_store_cloud_endpoint} \
        --eloq_store_cloud_access_key=${eloq_store_cloud_access_key} \
        --eloq_store_cloud_secret_key=${eloq_store_cloud_secret_key} \
        --eloq_store_cloud_store_path=${eloq_store_cloud_store_path} \
        --eloq_store_reuse_local_files=true \
        >/tmp/redis_server_single_node_nowal_small_ckpt_interval.log 2>&1 \
        &

    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_single_node.log

    run_tcl_tests all $build_type false true
    echo "finished small ckpt interval without wal and with data store." >>/tmp/redis_single_node.log
    echo "" >>/tmp/redis_single_node.log

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished
    #exit 0

    # run redis with wal and data store disabled.
    rm -rf ${eloq_data_path}/*

    echo "redirecting output to /tmp/ to prevent ci pipeline crash" >> /tmp/redis_single_node.log
    echo "default ckpt interval without wal and without data store." >> /tmp/redis_single_node.log
    ${eloqkv_bin_path} \
        --port=6379 \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=false \
	      --node_memory_limit_mb=${node_memory_limit_mb} \
        --eloq_data_path=${eloq_data_path} \
        --eloq_store_data_path_list=${eloq_store_data_path} \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=10 \
        --aws_access_key_id=${aws_access_key_id} \
        --aws_secret_key=${aws_secret_key} \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url=${txlog_rocksdb_cloud_s3_endpoint_url} \
	      --max_processing_time_microseconds=1000 \
	      --eloq_store_pages_per_file_shift=1 \
        --eloq_store_cloud_provider=${eloq_store_cloud_provider} \
        --eloq_store_cloud_endpoint=${eloq_store_cloud_endpoint} \
        --eloq_store_cloud_access_key=${eloq_store_cloud_access_key} \
        --eloq_store_cloud_secret_key=${eloq_store_cloud_secret_key} \
        --eloq_store_cloud_store_path=${eloq_store_cloud_store_path} \
        --eloq_store_reuse_local_files=true \
        >/tmp/redis_server_single_node_no_wal_no_data_store_default_ckpt_interval.log 2>&1 \
        &

    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_single_node.log

    run_tcl_tests all $build_type
    echo "finished default ckpt interval without wal and without data store." >>/tmp/redis_single_node.log

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    cleanup_minio_bucket ${ELOQSTORE_BUCKET_NAME}
    cleanup_minio_bucket ${ROCKSDB_CLOUD_BUCKET_NAME}
  fi

}

# Function to wait until data store server is ready
function wait_dss_until_ready() {
  local interval=1
  local timeout=300
  local elapsed=0
  local dss_log_path="/tmp/eloq_dss_data/eloq_dss_server.log"

  while [ $(grep -i "DataStoreService Server Started" ${dss_log_path} | wc -l) -eq 0 ]; do
    sleep $interval
    elapsed=$((elapsed + interval))
    echo "Wait until data store server ready for $elapsed seconds."
    if [ $elapsed -ge $timeout ]; then
      echo "Timeout: Data store server is not ready after $elapsed seconds."
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

  while [ $(ps aux | grep dss_server | grep -v grep | wc -l) -gt 0 ]; do
    sleep $interval
    elapsed=$((elapsed + interval))
    if [ $elapsed -ge $timeout ]; then
      echo "Timeout: Process still running after $timeout seconds."
      # list dss still alived
      ps aux | grep dss_server | grep -v grep
      return 1
    fi
  done
  return 0
}

function stop_and_clean_dss_server() {
  local kv_store_type=$1

  set +e
  pkill -x dss_server
  rm data_store_config.ini
  rm /tmp/data_store_config.ini
  rm -rf /tmp/eloq_dss_data
  set -e

  wait_dss_until_finished

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
        create_minio_bucket ${ELOQSTORE_BUCKET_NAME}
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
  local dss_node_memory_limit_mb=${DSS_NODE_MEMORY_LIMIT_MB:-1024}
  ${eloqkv_base_path}/data_substrate/store_handler/eloq_data_store_service/bld/dss_server \
    ${dss_server_configs} \
    --node_memory_limit_mb=${dss_node_memory_limit_mb} \
    --data_path=${dss_data_path} \
    --ip=$dss_ip \
    --port=$dss_port \
    --logtostderr=true \
    >${dss_log_path} 2>&1 \
    &
  local dss_server_pid=$!

  wait_dss_until_ready
  echo "dss_server is started, pid: $dss_server_pid"
}
function run_eloqkv_cluster_tests() {
  local build_type=$1
  local kv_store_type=$2
  local eloqkv_base_path="${ELOQKV_BASE_PATH}"

  # remove data dir generated by other tests.
  rm -rf /tmp/redis_server_data*

  cd ${eloqkv_base_path}

  if [[ $kv_store_type = "ROCKSDB" ]]; then
    echo "starting log service"
    local log_service_ip_port="127.0.0.1:9000"

    rm -rf /tmp/log_data
    ${ELOQKV_BASE_PATH}/install/bin/launch_sv \
      -conf=$log_service_ip_port \
      -node_id=0 \
      -storage_path="/tmp/log_data" \
      --logtostderr=true \
      >/tmp/redis_log_service.log 2>&1 \
      &

    local log_service_pid=$!
    echo "log_service is started, pid: $log_service_pid"
    # wait for log service to be ready
    sleep 10

    echo "bootstrap before start cluster to avoid contention"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=false \
      --eloq_data_path="/tmp/redis_server_data_0" \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --txlog_service_list=$log_service_ip_port \
      --txlog_group_replica_num=3 \
      --logtostderr=true \
      --bootstrap \
      --enable_io_uring=${enable_io_uring} \
      >/tmp/redis_server_multi_node_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    # wait for bootstrap to finish
    sleep 20

    # echo "bootstrap is started, pid: $!"
    # # wait for bootstrap to finish
    # sleep 20

    # pure memory mode does not need bootstrap
    # pure memory mode does not need log service

    echo "starting redis servers"
    local ports=(6379 7379 8379)
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash"
      env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
        ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
        --port=$port \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=false \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --rocksdb_storage_path="/tmp/rocksdb_data_$index" \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --checkpointer_interval=36000 \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --txlog_service_list=$log_service_ip_port \
        --txlog_group_replica_num=3 \
        --enable_io_uring=${enable_io_uring} \
        --logtostderr=true \
        >/tmp/redis_server_multi_node_$index.log 2>&1 \
        &
      redis_pids+=($!)
      echo "redis_server $index is started, pid: $!"
      index=$((index + 1))
    done

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    run_tcl_tests all $build_type true

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    kill $log_service_pid
    wait_until_finished

  elif [[ $kv_store_type = "ELOQDSS_ROCKSDB_CLOUD_S3" ]]; then
    echo "run_eloqkv_cluster_tests for ELOQDSS_ROCKSDB_CLOUD_S3"

    local rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}
    local rocksdb_cloud_aws_access_key_id=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID}
    local rocksdb_cloud_aws_secret_access_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}
    local rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME}
    local rocksdb_cloud_object_path=${ROCKSDB_CLOUD_OBJECT_PATH}
    local rocksdb_cloud_bucket_prefix=${ROCKSDB_CLOUD_BUCKET_PREFIX}
    local txlog_rocksdb_cloud_bucket_prefix=${ROCKSDB_CLOUD_BUCKET_PREFIX}
    local txlog_rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME}
    local txlog_rocksdb_cloud_object_path=${TXLOG_ROCKSDB_CLOUD_OBJECT_PATH}
    local txlog_rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}

    stop_and_clean_dss_server $kv_store_type
    start_dss_server "127.0.0.1" "9100" $kv_store_type
    local dss_server_ip_port="127.0.0.1:9100"

    # pure memory mode does not need log service

    rm -rf /tmp/redis_server_data*
    echo "starting redis servers"
    local ports=(6379 7379 8379)
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash"
      env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
        ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
        --port=$port \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=false \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --checkpointer_interval=36000 \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --txlog_service_list=$log_service_ip_port \
        --txlog_group_replica_num=3 \
        --eloq_dss_peer_node=$dss_server_ip_port \
        --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
        --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
        --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
        --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
        --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        --logtostderr=true \
        >/tmp/redis_server_multi_node_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers are started, pids: ${redis_pids[@]}"

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    run_tcl_tests all $build_type true

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    wait_until_finished

    # clean dss_server data and restart it.
    stop_and_clean_dss_server $kv_store_type
    start_dss_server "127.0.0.1" "9100" $kv_store_type
    rm -rf /tmp/redis_server_data*

    echo "bootstrap before start cluster to avoid contention"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=true \
      --eloq_data_path="/tmp/redis_server_data_bootstrap" \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --eloq_dss_peer_node=$dss_server_ip_port \
      --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
      --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
      --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --logtostderr=true \
      --bootstrap \
      >/tmp/redis_server_multi_node_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    # wait for bootstrap to finish
    sleep 20

    redis_pids=()
    local index=0
    for port in "${ports[@]}"; do
      env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
        ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
        --port=$port \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --eloq_dss_peer_node=$dss_server_ip_port \
        --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
        --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
        --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
        --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
        --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        --maxclients=1000000 \
        --logtostderr=true \
        >/tmp/redis_server_multi_node_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers are started, pids: ${redis_pids[@]}"

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    # wait for redis servers to be ready
    run_tcl_tests all $build_type true

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    wait_until_finished

    echo "starting log service"
    local log_service_ip_port="127.0.0.1:9000"

    rm -rf /tmp/log_data
    ${ELOQKV_BASE_PATH}/install/bin/launch_sv \
      -conf=$log_service_ip_port \
      -node_id=0 \
      -storage_path="/tmp/log_data" \
      --logtostderr=true \
      >/tmp/redis_log_service.log 2>&1 \
      &

    local log_service_pid=$!
    echo "log_service is started, pid: $log_service_pid"
    # wait for log service to be ready
    sleep 10

    # clean dss_server data and restart it.
    stop_and_clean_dss_server $kv_store_type
    start_dss_server "127.0.0.1" "9100" $kv_store_type
    rm -rf /tmp/redis_server_data*

    echo "bootstrap before start cluster to avoid contention"
    env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
      ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --eloq_data_path="/tmp/redis_server_data_bootstrap" \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --txlog_service_list=$log_service_ip_port \
      --txlog_group_replica_num=3 \
      --eloq_dss_peer_node=$dss_server_ip_port \
      --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
      --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
      --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --logtostderr=true \
      --bootstrap \
      >/tmp/redis_server_multi_node_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    # wait for bootstrap to finish
    sleep 20

    echo "starting redis servers"
    local ports=(6379 7379 8379)
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash"
      env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
        ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
        --port=$port \
        --core_number=2 \
        --enable_wal=true \
        --enable_data_store=true \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --checkpointer_interval=36000 \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --txlog_service_list=$log_service_ip_port \
        --txlog_group_replica_num=3 \
        --eloq_dss_peer_node=$dss_server_ip_port \
        --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
        --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
        --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
        --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
        --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        --logtostderr=true \
        >/tmp/redis_server_multi_node_$index.log 2>&1 \
        &
      redis_pids+=($!)
      echo "redis_server $index is started, pid: $!"
      index=$((index + 1))
    done

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"

    run_tcl_tests all $build_type true

    # TODO(ZX) log replay test for cluster
    echo "Running log replay test for Debug build: "

    local python_test_file="${eloqkv_base_path}/tests/unit/eloq/log_replay_test/log_replay_test.py"
    python3 $python_test_file --load > load.log 2>&1

    # wait for load to finish
    sleep 10

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill -9 $pid
      fi
    done

    # wait for kill to finish
    wait_until_finished

    # run redis instances again on different ports
    redis_pids=()
    local index=0
    for port in "${ports[@]}"; do
      env LD_LIBRARY_PATH=${ELOQKV_BASE_PATH}/install/lib/:${LD_LIBRARY_PATH} \
        ${ELOQKV_BASE_PATH}/install/bin/eloqkv \
        --port=$port \
        --core_number=2 \
        --enable_wal=true \
        --enable_data_store=true \
        --eloq_data_path="redis_server_data_$index" \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --checkpointer_interval=36000 \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --txlog_service_list=127.0.0.1:9000 \
        --txlog_group_replica_num=3 \
        --eloq_dss_peer_node=$dss_server_ip_port \
        --rocksdb_cloud_s3_endpoint_url="${rocksdb_cloud_s3_endpoint_url}" \
        --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
        --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name} \
        --rocksdb_cloud_object_path=${rocksdb_cloud_object_path} \
        --rocksdb_cloud_bucket_prefix=${rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        --logtostderr=true \
        >/tmp/redis_server_multi_node_no_wal_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done

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

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    kill $log_service_pid
    wait_until_finished

    # stop dss_server and clean bucket in minio
    stop_and_clean_dss_server $kv_store_type

  elif [[ $kv_store_type = "ELOQDSS_ELOQSTORE" ]]; then
    echo "eloqkv cluster test with dss_eloqstore." >/tmp/redis_cluster_with_eloqstore.log

    local node_memory_limit_mb=${NODE_MEMORY_LIMIT_MB:-2048}
    local dss_peer_node="127.0.0.1:9100"
    local rocksdb_cloud_aws_access_key_id=${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID}
    local rocksdb_cloud_aws_secret_access_key=${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY}
    local txlog_rocksdb_cloud_bucket_prefix=${ROCKSDB_CLOUD_BUCKET_PREFIX}
    local txlog_rocksdb_cloud_bucket_name=${ROCKSDB_CLOUD_BUCKET_NAME}
    local txlog_rocksdb_cloud_object_path=${TXLOG_ROCKSDB_CLOUD_OBJECT_PATH}
    local txlog_rocksdb_cloud_s3_endpoint_url=${ROCKSDB_CLOUD_S3_ENDPOINT}
    local eloq_store_cloud_store_path=${ELOQSTORE_BUCKET_NAME}/eloqstore
    local ports=(6379 7379 8379)
    local eloqkv_bin_path="${ELOQKV_BASE_PATH}/install/bin/eloqkv"

    # stop_and_clean_dss_server $kv_store_type
    # start_dss_server "127.0.0.1" "9100" $kv_store_type
    # pure memory mode does not need log service

    rm -rf /tmp/redis_server_data*

    #
    # pure memory mode does not need log service
    #
    rm -rf /tmp/redis_server_data_0/*
    rm -rf /tmp/redis_server_data_1/*
    rm -rf /tmp/redis_server_data_2/*
    echo "starting redis servers with nowal, nostore, pure memory." >>/tmp/redis_cluster_with_eloqstore.log
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >>/tmp/redis_cluster_with_eloqstore.log
      ${eloqkv_bin_path} \
        --port=$port \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=false \
        --log_dir="/tmp/redis_server_logs_$index" \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --node_memory_limit_mb=${node_memory_limit_mb} \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=36000 \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --txlog_group_replica_num=3 \
        --eloq_dss_peer_node=${dss_peer_node} \
        --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
        --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        >/tmp/redis_server_multi_node_nowal_nostore_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers with nowal, nostore, 36000 ckpt interval are started, pids: $redis_pids" >>/tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_cluster_with_eloqstore.log

    run_tcl_tests all $build_type true

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    wait_until_finished
    echo "finished redis_servers with nowal, nostore, pure memory." >>/tmp/redis_cluster_with_eloqstore.log
    echo "" >>/tmp/redis_cluster_with_eloqstore.log

    #
    # Test small ckpt interval
    #
    rm -rf /tmp/redis_server_data_0/*
    rm -rf /tmp/redis_server_data_1/*
    rm -rf /tmp/redis_server_data_2/*
    echo "bootstrap before start cluster to avoid contention" >>/tmp/redis_cluster_with_eloqstore.log
    ${eloqkv_bin_path} \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=true \
      --log_dir="/tmp/redis_server_logs_0" \
      --eloq_data_path="/tmp/redis_server_data_0" \
      --node_memory_limit_mb=${node_memory_limit_mb} \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --node_group_replica_num=1 \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
	    --max_processing_time_microseconds=1000 \
	    --eloq_store_pages_per_file_shift=1 \
      --eloq_store_cloud_endpoint="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --eloq_store_cloud_access_key="${rocksdb_cloud_aws_access_key_id}" \
      --eloq_store_cloud_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --eloq_store_cloud_store_path="${eloq_store_cloud_store_path}" \
      --eloq_store_reuse_local_files=true \
      --bootstrap \
      >/tmp/redis_server_multi_node_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    echo "bootstrap nowal, withstore is started, pid: $!" >>/tmp/redis_cluster_with_eloqstore.log
    # wait for bootstrap to finish
    sleep 20

    echo "starting redis servers nowal, withstore, small ckpt interval." >>/tmp/redis_cluster_with_eloqstore.log
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >>/tmp/redis_cluster_with_eloqstore.log
      ${eloqkv_bin_path} \
        --port=$port \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
        --log_dir="/tmp/redis_server_logs_$index" \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --node_memory_limit_mb=${node_memory_limit_mb} \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=1 \
        --kickout_data_for_test=true \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --node_group_replica_num=1 \
        --txlog_group_replica_num=3 \
        --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
        --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
	      --max_processing_time_microseconds=1000 \
	      --eloq_store_pages_per_file_shift=1 \
        --eloq_store_cloud_endpoint="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        --eloq_store_cloud_access_key="${rocksdb_cloud_aws_access_key_id}" \
        --eloq_store_cloud_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --eloq_store_cloud_store_path="${eloq_store_cloud_store_path}" \
        --eloq_store_reuse_local_files=true \
        >/tmp/redis_server_multi_node_nowal_withstore_smallckptinterval_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers with nowal, withstore, small ckpt interval are started, pids: $redis_pids" >>/tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_cluster_with_eloqstore.log

    run_tcl_tests all $build_type true true

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    wait_until_finished

    echo "stop data store server." >>/tmp/redis_cluster_with_eloqstore.log
    # kill data store server
    echo "finished redis_servers with nowal, withstore, small ckpt interval."  >> /tmp/redis_cluster_with_eloqstore.log
    echo ""  >> /tmp/redis_cluster_with_eloqstore.log

    #
    # Test default ckpt interval
    #
    rm -rf /tmp/redis_server_data_0/*
    rm -rf /tmp/redis_server_data_1/*
    rm -rf /tmp/redis_server_data_2/*
    echo "bootstrap before start cluster to avoid contention" >>/tmp/redis_cluster_with_eloqstore.log
    ${eloqkv_bin_path} \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=true \
      --log_dir="/tmp/redis_server_logs_0" \
      --eloq_data_path="/tmp/redis_server_data_0" \
      --node_memory_limit_mb=${node_memory_limit_mb} \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --node_group_replica_num=1 \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
	    --max_processing_time_microseconds=1000 \
	    --eloq_store_pages_per_file_shift=1 \
      --eloq_store_cloud_endpoint="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --eloq_store_cloud_access_key="${rocksdb_cloud_aws_access_key_id}" \
      --eloq_store_cloud_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --eloq_store_cloud_store_path="${eloq_store_cloud_store_path}" \
      --eloq_store_reuse_local_files=true \
      --bootstrap \
      >/tmp/redis_server_multi_node_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    echo "bootstrap nowal, withstore is started, pid: $!" >>/tmp/redis_cluster_with_eloqstore.log
    # wait for bootstrap to finish
    sleep 20

    redis_pids=()
    local index=0
    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >>/tmp/redis_cluster_with_eloqstore.log
      ${eloqkv_bin_path} \
        --port=$port \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
        --log_dir="/tmp/redis_server_logs_$index" \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --node_memory_limit_mb=${node_memory_limit_mb} \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --node_group_replica_num=1 \
        --txlog_group_replica_num=3 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --logtostderr=true \
        --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
        --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
	      --max_processing_time_microseconds=1000 \
	      --eloq_store_pages_per_file_shift=1 \
        --eloq_store_cloud_endpoint="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        --eloq_store_cloud_access_key="${rocksdb_cloud_aws_access_key_id}" \
        --eloq_store_cloud_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --eloq_store_cloud_store_path="${eloq_store_cloud_store_path}" \
        --eloq_store_reuse_local_files=true \
        >/tmp/redis_server_multi_node_nowal_withstore_defaultckptinterval_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers nowal, withstore, default ckpt interval are started, pids: $redis_pids" >>/tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_cluster_with_eloqstore.log

    # wait for redis servers to be ready
    run_tcl_tests all $build_type true

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    wait_until_finished

    echo "stop data store server." >> /tmp/redis_cluster_with_eloqstore.log
    echo "finished redis_servers nowal, withstore, default ckpt interval." >> /tmp/redis_cluster_with_eloqstore.log
    echo ""  >> /tmp/redis_cluster_with_eloqstore.log

    #
    # Test log replay
    #
    echo "starting log service" >>/tmp/redis_cluster_with_eloqstore.log
    local log_service_ip_port="127.0.0.1:9000"

    rm -rf /tmp/log_data
    ${ELOQKV_BASE_PATH}/install/bin/launch_sv \
      -conf=$log_service_ip_port \
      -node_id=0 \
      -storage_path="/tmp/log_data" \
      >/tmp/redis_log_service.log 2>&1 \
      &

    local log_service_pid=$!
    echo "log_service is started, pid: $log_service_pid"
    echo "log_service is started, pid: $log_service_pid" >>/tmp/redis_cluster_with_eloqstore.log
    # wait for log service to be ready
    sleep 10

    rm -rf /tmp/redis_server_data_0/*
    rm -rf /tmp/redis_server_data_1/*
    rm -rf /tmp/redis_server_data_2/*
    echo "bootstrap before start cluster to avoid contention" >>/tmp/redis_cluster_with_eloqstore.log
    ${eloqkv_bin_path} \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --log_dir="/tmp/redis_server_logs_0" \
      --eloq_data_path="/tmp/redis_server_data_0" \
      --node_memory_limit_mb=${node_memory_limit_mb} \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --node_group_replica_num=1 \
      --txlog_service_list=$log_service_ip_port \
      --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
      --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
      --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
      --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
      --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
	    --max_processing_time_microseconds=1000 \
	    --eloq_store_pages_per_file_shift=1 \
      --eloq_store_cloud_endpoint="${txlog_rocksdb_cloud_s3_endpoint_url}" \
      --eloq_store_cloud_access_key="${rocksdb_cloud_aws_access_key_id}" \
      --eloq_store_cloud_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
      --eloq_store_cloud_store_path="${eloq_store_cloud_store_path}" \
      --eloq_store_reuse_local_files=true \
      --bootstrap \
      >/tmp/redis_server_multi_node_withwal_withstore_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    echo "bootstrap is started, pid: $!" >>/tmp/redis_cluster_with_eloqstore.log
    # wait for bootstrap to finish
    sleep 20

    echo "starting redis servers before log replay" >>/tmp/redis_cluster_with_eloqstore.log
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >>/tmp/redis_cluster_with_eloqstore.log
      ${eloqkv_bin_path} \
        --port=$port \
        --core_number=2 \
        --enable_wal=true \
        --enable_data_store=true \
        --log_dir="/tmp/redis_server_logs_$index" \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --node_memory_limit_mb=${node_memory_limit_mb} \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=36000 \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --node_group_replica_num=1 \
        --txlog_service_list=$log_service_ip_port \
        --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
        --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
	      --max_processing_time_microseconds=1000 \
	      --eloq_store_pages_per_file_shift=1 \
        --eloq_store_cloud_endpoint="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        --eloq_store_cloud_access_key="${rocksdb_cloud_aws_access_key_id}" \
        --eloq_store_cloud_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --eloq_store_cloud_store_path="${eloq_store_cloud_store_path}" \
        --eloq_store_reuse_local_files=true \
        >/tmp/redis_server_multi_node_withwal_withstore_beforelogreplay_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers with wal, with datastore before log replay are started, pids: $redis_pids" >>/tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_cluster_with_eloqstore.log

    run_tcl_tests all $build_type true

    echo "Running log replay test for Debug build: " >>/tmp/redis_cluster_with_eloqstore.log

    local python_test_file="${eloqkv_base_path}/tests/unit/eloq/log_replay_test/log_replay_test.py"
    python3 $python_test_file --load > /tmp/load.log 2>&1

    # wait for load to finish
    sleep 10

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill -9 $pid
      fi
    done

    # wait for kill to finish
    wait_until_finished
    echo "finished redis_servers nowal, withstore before log replay." >>/tmp/redis_cluster_with_eloqstore.log
    echo "" >>/tmp/redis_cluster_with_eloqstore.log

    # run redis instances again on different ports
    echo "starting redis servers after log replay" >>/tmp/redis_cluster_with_eloqstore.log
    redis_pids=()
    local index=0
    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >>/tmp/redis_cluster_with_eloqstore.log
      ${eloqkv_bin_path} \
        --port=$port \
        --core_number=2 \
        --enable_wal=true \
        --enable_data_store=true \
        --log_dir="/tmp/redis_server_logs_$index" \
        --eloq_data_path="redis_server_data_$index" \
        --node_memory_limit_mb=${node_memory_limit_mb} \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=36000 \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --node_group_replica_num=1 \
        --txlog_service_list=$log_service_ip_port \
        --aws_access_key_id="${rocksdb_cloud_aws_access_key_id}" \
        --aws_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --txlog_rocksdb_cloud_bucket_prefix=${txlog_rocksdb_cloud_bucket_prefix} \
        --txlog_rocksdb_cloud_bucket_name=${txlog_rocksdb_cloud_bucket_name} \
        --txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path} \
        --txlog_rocksdb_cloud_s3_endpoint_url="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        --max_processing_time_microseconds=1000 \
        --eloq_store_pages_per_file_shift=1 \
        --cache_evict_policy=LO_LRU \
        --lolru_large_obj_threshold_kb=1 \
        --eloq_store_cloud_endpoint="${txlog_rocksdb_cloud_s3_endpoint_url}" \
        --eloq_store_cloud_access_key="${rocksdb_cloud_aws_access_key_id}" \
        --eloq_store_cloud_secret_key="${rocksdb_cloud_aws_secret_access_key}" \
        --eloq_store_cloud_store_path="${eloq_store_cloud_store_path}" \
        --eloq_store_reuse_local_files=true \
        >/tmp/redis_server_multi_node_withwal_withstore_afterlogreplay_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers with wal, with datastore after log replay are started, pids: $redis_pids" >>/tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >>/tmp/redis_cluster_with_eloqstore.log

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

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    kill $log_service_pid
    wait_until_finished

    echo "finished redis_servers nowal, withstore after log replay." >> /tmp/redis_cluster_with_eloqstore.log

  fi
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

    # run standby tests.
    python3 run_tests.py --dbtype redis --group standby --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path} --bootstrap true
    rm -rf runtime/*

    # rm -rf runtime/*
    # python3 redis_test/datastore_test/datastore_scale_test.py --dbtype redis --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
    python3 run_tests.py --dbtype redis --group single --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}

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
    python3 run_tests.py --dbtype redis --group single --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path}

    # run single/multi test
    rm -rf runtime/*
    prepare_eloqstore_minio_buckets
    python3 redis_test/multi_test/smoke_test.py --dbtype redis --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path} --bootstrap true

    prepare_eloqstore_minio_buckets
    python3 redis_test/multi_test/cluster_rolling_upgrade.py --dbtype redis --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path} --bootstrap true
    prepare_eloqstore_minio_buckets
    python3 redis_test/multi_test/cluster_scale_test.py --dbtype redis --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path} --bootstrap true

    # run log service scale test
    rm -rf runtime/*
    prepare_eloqstore_minio_buckets
    python3 redis_test/log_service_test/log_service_scale_test.py --dbtype redis --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path}

    # run standby test
    rm -rf runtime/*
    prepare_eloqstore_minio_buckets
    python3 run_tests.py --dbtype redis --group standby --storage eloqdss-eloqstore-local --install_path ${eloqkv_install_path} --bootstrap true
    rm -rf runtime/*
    prepare_eloqstore_minio_buckets
    python3 run_tests.py --dbtype redis --group standby --storage eloqdss-eloqstore-cloud --install_path ${eloqkv_install_path} --bootstrap true
    rm -rf runtime/*
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
