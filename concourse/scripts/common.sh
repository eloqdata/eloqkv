#!/bin/bash

cat << EOF
################################################################################
#If the lengthy task output renders extremely slow,please try fly watch command#
#(https://concoursetutorial.com/basics/watch-job-output.html)                  #
################################################################################
EOF

function kernel_version_greater_than_6.5() {
  kernel_version=$(uname -r)
  major=$(echo "$kernel_version" | cut -d. -f1)
  minor=$(echo "$kernel_version" | cut -d. -f2)

  if (( major > 6 )) || { (( major == 6 )) && (( minor >= 5 )); }; then
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
  local timeout=120
  local elapsed=0
  local interval=1

  while [ $(ps aux | grep eloqkv | grep -v grep | grep -v launch_sv | grep -v dss_server | wc -l) -gt 0 ]; do
    sleep $interval
    elapsed=$((elapsed + interval))
    if [ $elapsed -ge $timeout ]; then
      echo "Timeout: Process still running after 20 seconds."
      # list eloqkv still alived
      ps aux | grep eloqkv | grep -v grep | grep -v launch_sv | grep -v dss_server
      return 1
    fi
  done
  return 0
}

function run_tcl_tests()
{
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

  local eloqkv_base_path="/home/$current_user/workspace/eloqkv"

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
    --single /unit/mono/"
  local files=$(find ${eloqkv_base_path}/tests/unit/mono -maxdepth 2 -type f)

  for file in $files; do
    local file_extension="${file##*.}"
    local relative_path="${file#${eloqkv_base_path}/tests/unit/mono/}"
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

function cleanup_minio_bucket()
{
  bucket_name=$1
  #prefix="eloqkv-"
  bucket_full_name="eloqkv-${bucket_name}"
  SCRIPT_DIR="/home/$current_user/workspace/eloqkv/concourse/scripts"
  echo "Clean up bucket ${bucket_name}"
  python3 ${SCRIPT_DIR}/cleanup_minio_bucket.py \
               --minio_endpoint ${ROCKSDB_CLOUD_S3_ENDPOINT} \
               --minio_access_key ${ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID} \
               --minio_secret_key ${ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY} \
               --bucket_name ${bucket_full_name}
}

function run_build() {
  local build_type=$1
  local kv_store_type=$2

  # compile eloqkv
  cd /home/$current_user/workspace/eloqkv
  cmake \
    -S /home/$current_user/workspace/eloqkv \
    -B /home/$current_user/workspace/eloqkv/cmake \
    -DCMAKE_INSTALL_PREFIX=/home/$current_user/workspace/eloqkv/install \
    -DCMAKE_BUILD_TYPE=$build_type \
    -DWITH_DATA_STORE=$kv_store_type \
    -DELOQ_MODULE_ENABLED=ON \
    -DEXT_TX_PROC_ENABLED=ON \
    -DBUILD_WITH_TESTS=ON \
    -DWITH_LOG_SERVICE=ON

  # Define the output log file
  log_file="/tmp/compile_info.log"

  # Function to run cmake build and check for errors
  run_cmake_build() {
    local target=$1
    cmake --build /home/$current_user/workspace/eloqkv/cmake --target "$target" -j 8
    local exit_status=$?

    if [ $exit_status -ne 0 ]; then
      echo "CMake build for target '$target' failed."
      exit $exit_status
    fi

    cmake --install /home/$current_user/workspace/eloqkv/cmake
  }

  # Run builds for the specified targets
  targets=("eloqkv" "object_serialize_deserialize_test")

  set +e
  for target in "${targets[@]}"; do
    run_cmake_build "$target"
  done
  set -e

  # compile log service to setup redis cluster later
  cd /home/$current_user/workspace/eloqkv/data_substrate/log_service
  cmake -B bld -DCMAKE_BUILD_TYPE=$build_type && cmake --build bld -j 8
  cp /home/$current_user/workspace/eloqkv/data_substrate/log_service/bld/launch_sv  /home/$current_user/workspace/eloqkv/install/bin/

case "$kv_store_type" in
  ELOQDSS_*)
      echo "build dss_server"
      cd /home/$current_user/workspace/eloqkv/data_substrate/store_handler/eloq_data_store_service
      cmake -B bld -DCMAKE_BUILD_TYPE=$build_type -DWITH_DATA_STORE=$kv_store_type && cmake --build bld -j8
      cp /home/$current_user/workspace/eloqkv/data_substrate/store_handler/eloq_data_store_service/bld/dss_server  /home/$current_user/workspace/eloqkv/install/bin/
      ;;
esac

  cd /home/$current_user/workspace/eloqkv

}

function run_build_ent() {
  local build_type=$1
  local kv_store_type=$2
  local txlog_log_state=$3

  # compile eloqkv
  cd /home/$current_user/workspace/eloqkv
  cmake \
    -S /home/$current_user/workspace/eloqkv \
    -B /home/$current_user/workspace/eloqkv/cmake \
    -DCMAKE_INSTALL_PREFIX=/home/$current_user/workspace/eloqkv/install \
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
    cmake --build /home/$current_user/workspace/eloqkv/cmake -j 8
    local exit_status=$?

    if [ $exit_status -ne 0 ]; then
      echo "CMake build failed."
      exit $exit_status
    fi
  }

  set +e
  run_cmake_build
  set -e

  cmake --install /home/$current_user/workspace/eloqkv/cmake

  # compile log service to setup redis cluster later
  cd /home/$current_user/workspace/eloqkv/data_substrate/eloq_log_service
  cmake -B bld -DCMAKE_BUILD_TYPE=$build_type && cmake --build bld -j 8
  cp /home/$current_user/workspace/eloqkv/data_substrate/eloq_log_service/bld/launch_sv  /home/$current_user/workspace/eloqkv/install/bin/

case "$kv_store_type" in
  ELOQDSS_*)
      echo "build dss_server"
      cd /home/$current_user/workspace/eloqkv/data_substrate/store_handler/eloq_data_store_service
      cmake -B bld -DCMAKE_BUILD_TYPE=$build_type -DWITH_DATA_STORE=$kv_store_type && cmake --build bld -j8
      cp /home/$current_user/workspace/eloqkv/data_substrate/store_handler/eloq_data_store_service/bld/dss_server  /home/$current_user/workspace/eloqkv/install/bin/
      ;;
esac

  cd /home/$current_user/workspace/eloqkv

}

function run_eloq_ttl_tests() {
  #TestsWithMem TestsWithKV TestsWithLog
  local test_case=$1
  local enable_wal=$2
  local enable_data_store=$3
  local kv_type=$4

  local timestamp=$(($(date +%s%N) / 1000000))
  local keyspace_name="redis_test_${timestamp}"
  echo "keyspace name is, ${keyspace_name}"

  cd /home/$current_user/workspace/eloqkv
  local exe_path="/home/$current_user/workspace/eloqkv/install/bin/eloqkv"
  local python_test_file="/home/$current_user/workspace/eloq_test/redis_test/single_test/test_ttl_eloqkv.py"
  python3 $python_test_file $exe_path ${test_case} --enable_wal=${enable_wal} --enable_data_store=${enable_data_store} --kv_type=${kv_type} --cass_host=${CASS_HOST} --cass_keyspace=${keyspace_name} --cass_bin="/home/$current_user/workspace/apache-cassandra-4.0.6/bin/"

}

function run_eloqkv_tests() {
  local build_type=$1
  local kv_store_type=$2
  local eloqkv_base_path="/home/$current_user/workspace/eloqkv"

  # clean data dir
  rm -rf /tmp/eloq_data

  cd ${eloqkv_base_path}

  if [[ $kv_store_type = "CASSANDRA" ]]; then
    # generate cassandra keyspace name.
    local timestamp=$(($(date +%s%N) / 1000000))
    local keyspace_name="redis_test_${timestamp}"
    echo "cassandra keyspace name is, ${keyspace_name}"
    /home/$current_user/workspace/apache-cassandra-4.0.6/bin/cqlsh $CASS_HOST -e "DROP KEYSPACE IF EXISTS $keyspace_name;"

    # run redis
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --cass_hosts=$CASS_HOST \
      --cass_keyspace=$keyspace_name \
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

    local python_test_file="/home/$current_user/workspace/eloqkv/tests/unit/mono/log_replay_test/log_replay_test.py"
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --cass_hosts=$CASS_HOST \
      --cass_keyspace=$keyspace_name \
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

    cd /home/$current_user/workspace/eloqkv
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
        --port=6379 \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
        --cass_hosts=$CASS_HOST \
        --cass_keyspace=$keyspace_name \
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

    # drop cassandra keyspace
    /home/$current_user/workspace/apache-cassandra-4.0.6/bin/cqlsh $CASS_HOST -e "DROP KEYSPACE IF EXISTS $keyspace_name;"

    # run redis with wal and data store disabled.
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

    run_tcl_tests all $build_type

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished
  elif [[ $kv_store_type = "ROCKSDB" ]]; then

    echo "bootstrap rocksdb"

    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

    local python_test_file="/home/$current_user/workspace/eloqkv/tests/unit/mono/log_replay_test/log_replay_test.py"
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

    cd /home/$current_user/workspace/eloqkv
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

  elif [[ $kv_store_type = "DYNAMODB" ]]; then
    # generate dynamodb keyspace name.
    local timestamp=$(($(date +%s%N) / 1000000))
    local keyspace_name="redis_test_${timestamp}"
    echo "dynamodb keyspace name is, ${keyspace_name}"


    local dynamodb_endpoint=${DYNAMO_ENDPOINT}
    local dynamodb_region=${AWS_DEFAULT_REGION}
    local aws_access_key_id=${AWS_ACCESS_KEY_ID}
    local aws_secret_key=${AWS_SECRET_ACCESS_KEY}


    # run redis
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --dynamodb_endpoint=$dynamodb_endpoint \
      --dynamodb_region=$dynamodb_region \
      --aws_access_key_id=$aws_access_key_id \
      --aws_secret_key=$aws_secret_key \
      --dynamodb_keyspace=$keyspace_name \
      --maxclients=1000000 \
      --checkpointer_interval=10 \
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

    local python_test_file="/home/$current_user/workspace/eloqkv/tests/unit/mono/log_replay_test/log_replay_test.py"
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --dynamodb_endpoint=$dynamodb_endpoint \
      --dynamodb_region=$dynamodb_region \
      --aws_access_key_id=$aws_access_key_id \
      --aws_secret_key=$aws_secret_key \
      --dynamodb_keyspace=$keyspace_name \
      --maxclients=1000000 \
      --checkpointer_interval=10 \
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

    cd /home/$current_user/workspace/eloqkv
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
        --port=6379 \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
        --dynamodb_endpoint=$dynamodb_endpoint \
        --dynamodb_region=$dynamodb_region \
        --aws_access_key_id=$aws_access_key_id \
        --aws_secret_key=$aws_secret_key \
        --dynamodb_keyspace=$keyspace_name \
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

    # drop keyspace

    # run redis with wal and data store disabled.
    echo "redirecting output to /tmp/ to prevent ci pipeline crash"
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

    local python_test_file="/home/$current_user/workspace/eloqkv/tests/unit/mono/log_replay_test/log_replay_test.py"
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

    cd /home/$current_user/workspace/eloqkv
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
    echo "single eloqkv node with dss_eloqstore." > /tmp/redis_single_node.log

    local eloq_data_path="/tmp/eloqkv_data"
    local node_memory_limit_mb=8192
    local eloq_store_worker_num=2
    local eloq_store_data_path="/tmp/eloqkv_data/eloq_store"
    local eloqkv_bin_path="/home/$current_user/workspace/eloqkv/install/bin/eloqkv"

    # run redis with small ckpt interval.
    rm -rf ${eloq_data_path}/*
    echo "redirecting output to /tmp/ to prevent ci pipeline crash" >> /tmp/redis_single_node.log
    echo "small ckpt interval with wal and data store." >> /tmp/redis_single_node.log
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    ${eloqkv_bin_path} \
        --port=6379 \
        --core_number=2 \
        --enable_wal=true \
        --enable_data_store=true \
        --node_memory_limit_mb=${node_memory_limit_mb} \
        --eloq_data_path=${eloq_data_path} \
        --eloq_store_worker_num=${eloq_store_worker_num} \
        --eloq_store_data_path=${eloq_store_data_path} \
        --maxclients=1000000 \
	      --logtostderr=true \
        --checkpointer_interval=1 \
	      --kickout_data_for_test=true \
        >/tmp/redis_server_single_node_small_ckpt_interval.log 2>&1 \
        &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >> /tmp/redis_single_node.log

    run_tcl_tests all $build_type false true

    echo "finished small ckpt interval with wal and data store." >> /tmp/redis_single_node.log
    echo "" >> /tmp/redis_single_node.log
    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished

    # run redis
    rm -rf ${eloq_data_path}/*
    echo "redirecting output to /tmp/ to prevent ci pipeline crash" >> /tmp/redis_single_node.log
    echo "big ckpt interval before replay with wal and data store." >> /tmp/redis_single_node.log
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    ${eloqkv_bin_path} \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --node_memory_limit_mb=${node_memory_limit_mb} \
      --eloq_data_path=${eloq_data_path} \
      --eloq_store_worker_num=${eloq_store_worker_num} \
      --eloq_store_data_path=${eloq_store_data_path} \
      --maxclients=1000000 \
      --logtostderr=true \
      --checkpointer_interval=36000 \
      >/tmp/redis_server_single_node_before_replay.log 2>&1 \
      &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >> /tmp/redis_single_node.log

    run_tcl_tests all $build_type
    echo "finished big ckpt interval before replay with wal and data store." >> /tmp/redis_single_node.log
    echo "" >> /tmp/redis_single_node.log

    # log replay test
    echo "Running log replay test for $build_type build: " >> /tmp/redis_single_node.log
    echo "" >> /tmp/redis_single_node.log

    local python_test_file="${eloqkv_base_path}/tests/unit/mono/log_replay_test/log_replay_test.py"
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
    echo "redirecting output to /tmp/ to prevent ci pipeline crash" >> /tmp/redis_single_node.log
    echo "big ckpt interval after replay with wal and data store." >> /tmp/redis_single_node.log
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    ${eloqkv_bin_path} \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --node_memory_limit_mb=${node_memory_limit_mb} \
      --eloq_data_path=${eloq_data_path} \
      --eloq_store_worker_num=${eloq_store_worker_num} \
      --eloq_store_data_path=${eloq_store_data_path} \
      --maxclients=1000000 \
      --logtostderr=true \
      --checkpointer_interval=36000 \
      >/tmp/redis_server_single_node_after_replay.log 2>&1 \
      &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >> /tmp/redis_single_node.log

    echo "verify log replay result" >> /tmp/redis_single_node.log
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

    echo "finished big ckpt interval after replay with wal and data store." >> /tmp/redis_single_node.log
    echo "" >> /tmp/redis_single_node.log
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
        --eloq_store_worker_num=${eloq_store_worker_num} \
        --eloq_store_data_path=${eloq_store_data_path} \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=10 \
        >/tmp/redis_server_single_node_no_wal_default_ckpt_interval.log 2>&1 \
        &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >> /tmp/redis_single_node.log

    run_tcl_tests all $build_type

    echo "finished default ckpt interval without wal and with data store." >> /tmp/redis_single_node.log
    echo "" >> /tmp/redis_single_node.log
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
        --eloq_store_worker_num=${eloq_store_worker_num} \
        --eloq_store_data_path=${eloq_store_data_path} \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=1 \
	      --kickout_data_for_test=true \
        >/tmp/redis_server_single_node_nowal_small_ckpt_interval.log 2>&1 \
        &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >> /tmp/redis_single_node.log

    run_tcl_tests all $build_type false true
    echo "finished samll ckpt interval without wal and with data store." >> /tmp/redis_single_node.log
    echo "" >> /tmp/redis_single_node.log

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
        --eloq_store_worker_num=${eloq_store_worker_num} \
        --eloq_store_data_path=${eloq_store_data_path} \
        --maxclients=1000000 \
        --logtostderr=true \
        --checkpointer_interval=10 \
        >/tmp/redis_server_single_node_no_wal_no_data_store_default_ckpt_interval.log 2>&1 \
        &
    local redis_pid=$!

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >> /tmp/redis_single_node.log

    run_tcl_tests all $build_type
    echo "finished default ckpt interval without wal and without data store." >> /tmp/redis_single_node.log

    # kill redis_server
    if [[ -n $redis_pid && -e /proc/$redis_pid ]]; then
      kill $redis_pid
    fi

    # wait for kill to finish
    wait_until_finished
  fi

}

# Function to wait until data store server is ready
function wait_dss_until_ready(){
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
    local eloqkv_base_path="/home/$current_user/workspace/eloqkv"
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
        local eloq_store_open_files_limit=10240
        dss_server_configs="--eloq_store_worker_num=${eloq_store_worker_num} \
                            --eloq_store_data_path=${eloq_store_data_path} \
                            --eloq_store_open_files_limit=${eloq_store_open_files_limit}"
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
      > ${dss_log_path} 2>&1 \
      &
    local dss_server_pid=$!

    wait_dss_until_ready
    echo "dss_server is started, pid: $dss_server_pid"
}
function run_eloqkv_cluster_tests() {
  local build_type=$1
  local kv_store_type=$2
  local eloqkv_base_path="/home/$current_user/workspace/eloqkv"

  # remove data dir generated by other tests.
  rm -rf /tmp/redis_server_data*

  cd ${eloqkv_base_path}

  if [[ $kv_store_type = "CASSANDRA" ]]; then

    # pure memory mode does not need log service

    rm -rf /tmp/redis_server_data*
    echo "starting redis servers"
    local ports=(6379 7379 8379)
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash"
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
        --cass_hosts=$CASS_HOST \
        --cass_port=9042 \
        --cass_user=cassandra \
        --cass_password=cassandra \
        --cass_keyspace=$keyspace_name \
        --cass_keyspace_class=SimpleStrategy \
        --cass_keyspace_replication=1 \
        --enable_io_uring=${enable_io_uring} \
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

    # generate cassandra keyspace name.
    local timestamp=$(($(date +%s%N)/1000000))
    local keyspace_name="redis_test_${timestamp}"
    echo "cassandra keyspace name is, ${keyspace_name}"
    rm -rf /tmp/redis_server_data*
    /home/$current_user/workspace/apache-cassandra-4.0.6/bin/cqlsh $CASS_HOST -e "DROP KEYSPACE IF EXISTS $keyspace_name;"

    echo "bootstrap before start cluster to avoid contention"
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=true \
      --eloq_data_path="/tmp/redis_server_data_0" \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --cass_hosts=$CASS_HOST \
      --cass_port=9042 \
      --cass_user=cassandra \
      --cass_password=cassandra \
      --cass_keyspace=$keyspace_name \
      --cass_keyspace_class=SimpleStrategy \
      --cass_keyspace_replication=1 \
      --logtostderr=true \
      --bootstrap \
      --enable_io_uring=${enable_io_uring} \
      >/tmp/redis_server_multi_node_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    # wait for bootstrap to finish
    sleep 20



    redis_pids=()
    local index=0
    for port in "${ports[@]}"; do
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
        --port=$port \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --cass_hosts=$CASS_HOST \
        --cass_keyspace=$keyspace_name \
        --cass_port=9042 \
        --cass_user=cassandra \
        --cass_password=cassandra \
        --cass_keyspace=$keyspace_name \
        --cass_keyspace_class=SimpleStrategy \
        --cass_keyspace_replication=1 \
        --enable_io_uring=${enable_io_uring} \
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
    /home/$current_user/workspace/eloqkv/install/bin/launch_sv \
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

    rm -rf /tmp/redis_server_data*
    /home/$current_user/workspace/apache-cassandra-4.0.6/bin/cqlsh $CASS_HOST -e "DROP KEYSPACE IF EXISTS $keyspace_name;"

    echo "bootstrap before start cluster to avoid contention"
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --eloq_data_path="/tmp/redis_server_data_0" \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=36000 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --txlog_service_list=$log_service_ip_port \
      --txlog_group_replica_num=3 \
      --cass_hosts=$CASS_HOST \
      --cass_port=9042 \
      --cass_user=cassandra \
      --cass_password=cassandra \
      --cass_keyspace=$keyspace_name \
      --cass_keyspace_class=SimpleStrategy \
      --cass_keyspace_replication=1 \
      --logtostderr=true \
      --bootstrap \
      --enable_io_uring=${enable_io_uring} \
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
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
        --cass_hosts=$CASS_HOST \
        --cass_port=9042 \
        --cass_user=cassandra \
        --cass_password=cassandra \
        --cass_keyspace=$keyspace_name \
        --cass_keyspace_class=SimpleStrategy \
        --cass_keyspace_replication=1 \
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

    # TODO(ZX) log replay test for cluster
    echo "Running log replay test for Debug build: "

    local python_test_file="/home/$current_user/workspace/eloqkv/tests/unit/mono/log_replay_test/log_replay_test.py"
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
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
        --cass_hosts=$CASS_HOST \
        --cass_port=9042 \
        --cass_user=cassandra \
        --cass_password=cassandra \
        --cass_keyspace=$keyspace_name \
        --cass_keyspace_class=SimpleStrategy \
        --cass_keyspace_replication=1 \
        --enable_io_uring=${enable_io_uring} \
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

    file1="database_snapshot_before_replay.json"  # First JSON file
    file2="database_snapshot_after_replay.json"  # Second JSON file

    if [[ -z "$file1" || -z "$file2" ]]; then
        echo "ERROR: database_snapshot_before_replay.json or database_snapshot_after_replay.json not generated"
        exit 1
    fi

    # Sort JSON content and compare using diff
    if diff <(jq -S . "$file1") <(jq -S . "$file2") &> /dev/null; then
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
    # drop cassandra keyspace
    /home/$current_user/workspace/apache-cassandra-4.0.6/bin/cqlsh $CASS_HOST -e "DROP KEYSPACE IF EXISTS $keyspace_name;"

  elif [[ $kv_store_type = "ROCKSDB" ]]; then
    echo "starting log service"
    local log_service_ip_port="127.0.0.1:9000"

    rm -rf /tmp/log_data
    /home/$current_user/workspace/eloqkv/install/bin/launch_sv \
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

  elif [[ $kv_store_type = "DYNAMODB" ]]; then

    # pure memory mode does not need log service

    rm -rf /tmp/redis_server_data*
    echo "starting redis servers"
    local ports=(6379 7379 8379)
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash"
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
        --enable_io_uring=${enable_io_uring} \
        --logtostderr=true \
        >/tmp/redis_server_multi_node_pure_mem_$index.log 2>&1 \
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


    # generate dynamo keyspace name.
    local timestamp=$(($(date +%s%N)/1000000))
    local keyspace_name="redis_test_${timestamp}"
    echo "dynamo keyspace name is, ${keyspace_name}"
    rm -rf /tmp/redis_server_data*

    local dynamodb_endpoint=${DYNAMO_ENDPOINT}
    local dynamodb_region=${AWS_DEFAULT_REGION}
    local aws_access_key_id=${AWS_ACCESS_KEY_ID}
    local aws_secret_key=${AWS_SECRET_ACCESS_KEY}

    echo "bootstrap before start cluster to avoid contention"
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=false \
      --enable_data_store=true \
      --eloq_data_path="/tmp/redis_server_data_0" \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=10 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --dynamodb_endpoint=$dynamodb_endpoint \
      --dynamodb_region=$dynamodb_region \
      --aws_access_key_id=$aws_access_key_id \
      --aws_secret_key=$aws_secret_key \
      --dynamodb_keyspace=$keyspace_name \
      --logtostderr=true \
      --bootstrap \
      --enable_io_uring=${enable_io_uring} \
      >/tmp/redis_server_multi_node_bootstrap_with_kv.log 2>&1


    echo "bootstrap is started, pid: $!"
    # wait for bootstrap to finish
    sleep 20


    local index=0
    redis_pids=()
    for port in "${ports[@]}"; do
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
        --port=$port \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --dynamodb_endpoint=$dynamodb_endpoint \
        --dynamodb_region=$dynamodb_region \
        --aws_access_key_id=$aws_access_key_id \
        --aws_secret_key=$aws_secret_key \
        --dynamodb_keyspace=$keyspace_name \
        --checkpointer_interval=10 \
        --logtostderr=true \
        --maxclients=1000000 \
        --enable_io_uring=${enable_io_uring} \
        --logtostderr=true \
        >/tmp/redis_server_multi_node_with_kv_$index.log 2>&1 \
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
    /home/$current_user/workspace/eloqkv/install/bin/launch_sv \
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

    rm -rf /tmp/redis_server_data*
    # TODO: drop keyspace
    local timestamp=$(($(date +%s%N)/1000000))
    local keyspace_name="redis_test_${timestamp}"
    echo "dynamo keyspace name is, ${keyspace_name}"

    echo "bootstrap before start cluster to avoid contention"
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
      --port=6379 \
      --core_number=2 \
      --enable_wal=true \
      --enable_data_store=true \
      --eloq_data_path="/tmp/redis_server_data_0" \
      --event_dispatcher_num=1 \
      --auto_redirect=true \
      --maxclients=1000000 \
      --checkpointer_interval=10 \
      --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
      --txlog_service_list=$log_service_ip_port \
      --txlog_group_replica_num=3 \
      --dynamodb_endpoint=$dynamodb_endpoint \
      --dynamodb_region=$dynamodb_region \
      --aws_access_key_id=$aws_access_key_id \
      --aws_secret_key=$aws_secret_key \
      --dynamodb_keyspace=$keyspace_name \
      --logtostderr=true \
      --bootstrap \
      --enable_io_uring=${enable_io_uring} \
      >/tmp/redis_server_multi_node_bootstrap_with_wal.log 2>&1


    echo "bootstrap is started, pid: $!"
    # wait for bootstrap to finish
    sleep 20

    echo "starting redis servers"
    local ports=(6379 7379 8379)
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash"
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
        --port=$port \
        --core_number=2 \
        --enable_wal=true \
        --enable_data_store=true \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --checkpointer_interval=10 \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --txlog_service_list=$log_service_ip_port \
        --txlog_group_replica_num=3 \
        --dynamodb_endpoint=$dynamodb_endpoint \
        --dynamodb_region=$dynamodb_region \
        --aws_access_key_id=$aws_access_key_id \
        --aws_secret_key=$aws_secret_key \
        --dynamodb_keyspace=$keyspace_name \
        --logtostderr=true \
        --enable_io_uring=${enable_io_uring} \
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

    local python_test_file="/home/$current_user/workspace/eloqkv/tests/unit/mono/log_replay_test/log_replay_test.py"
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
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
        --port=$port \
        --core_number=2 \
        --enable_wal=true \
        --enable_data_store=true \
        --eloq_data_path="redis_server_data_$index" \
        --event_dispatcher_num=1 \
        --auto_redirect=true \
        --maxclients=1000000 \
        --checkpointer_interval=10 \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --txlog_service_list=$log_service_ip_port \
        --txlog_group_replica_num=3 \
        --dynamodb_endpoint=$dynamodb_endpoint \
        --dynamodb_region=$dynamodb_region \
        --aws_access_key_id=$aws_access_key_id \
        --aws_secret_key=$aws_secret_key \
        --dynamodb_keyspace=$keyspace_name \
        --logtostderr=true \
        --enable_io_uring=${enable_io_uring} \
        >/tmp/redis_server_multi_node_with_wal_$index.log 2>&1 \
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

    file1="database_snapshot_before_replay.json"  # First JSON file
    file2="database_snapshot_after_replay.json"  # Second JSON file

    if [[ -z "$file1" || -z "$file2" ]]; then
        echo "ERROR: database_snapshot_before_replay.json or database_snapshot_after_replay.json not generated"
        exit 1
    fi

    # Sort JSON content and compare using diff
    if diff <(jq -S . "$file1") <(jq -S . "$file2") &> /dev/null; then
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
    # drop dynamodb keyspace


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
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
    /home/$current_user/workspace/eloqkv/install/bin/launch_sv \
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
    env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
    /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

    local python_test_file="/home/$current_user/workspace/eloqkv/tests/unit/mono/log_replay_test/log_replay_test.py"
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
      env LD_LIBRARY_PATH=/home/$current_user/workspace/eloqkv/install/lib/:${LD_LIBRARY_PATH} \
      /home/$current_user/workspace/eloqkv/install/bin/eloqkv \
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

    file1="database_snapshot_before_replay.json"  # First JSON file
    file2="database_snapshot_after_replay.json"  # Second JSON file

    if [[ -z "$file1" || -z "$file2" ]]; then
        echo "ERROR: database_snapshot_before_replay.json or database_snapshot_after_replay.json not generated"
        exit 1
    fi

    # Sort JSON content and compare using diff
    if diff <(jq -S . "$file1") <(jq -S . "$file2") &> /dev/null; then
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
    echo "eloqkv cluster test with dss_eloqstore." > /tmp/redis_cluster_with_eloqstore.log

    local node_memory_limit_mb=8192
    local dss_peer_node="127.0.0.1:9100"
    local ports=(6379 7379 8379)
    local eloqkv_bin_path="/home/$current_user/workspace/eloqkv/install/bin/eloqkv"

    #
    # pure memory mode does not need log service
    #
    rm -rf /tmp/redis_server_data_0/*
    rm -rf /tmp/redis_server_data_1/*
    rm -rf /tmp/redis_server_data_2/*
    echo "starting redis servers with nowal, nostore, pure memory." >> /tmp/redis_cluster_with_eloqstore.log
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >> /tmp/redis_cluster_with_eloqstore.log
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
        >/tmp/redis_server_multi_node_nowal_nostore_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers with nowal, nostore, 36000 ckpt interval are started, pids: $redis_pids"  >> /tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >> /tmp/redis_cluster_with_eloqstore.log

    run_tcl_tests all $build_type true

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    wait_until_finished
    echo "finished redis_servers with nowal, nostore, pure memory."  >> /tmp/redis_cluster_with_eloqstore.log
    echo "" >> /tmp/redis_cluster_with_eloqstore.log

    #
    # Test small ckpt interval
    #
    start_dss_server "127.0.0.1" "9100" $kv_store_type
    echo "Data store server is ready!"  >> /tmp/redis_cluster_with_eloqstore.log

    rm -rf /tmp/redis_server_data_0/*
    rm -rf /tmp/redis_server_data_1/*
    rm -rf /tmp/redis_server_data_2/*
    echo "bootstrap before start cluster to avoid contention" >> /tmp/redis_cluster_with_eloqstore.log
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
      --eloq_dss_peer_node=${dss_peer_node} \
      --bootstrap \
      >/tmp/redis_server_multi_node_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    echo "bootstrap nowal, withstore is started, pid: $!"  >> /tmp/redis_cluster_with_eloqstore.log
    # wait for bootstrap to finish
    sleep 20

    echo "starting redis servers nowal, withstore, small ckpt interval." >> /tmp/redis_cluster_with_eloqstore.log
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >> /tmp/redis_cluster_with_eloqstore.log
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
        --txlog_group_replica_num=3 \
        --eloq_dss_peer_node=${dss_peer_node} \
        >/tmp/redis_server_multi_node_nowal_withstore_smallckptinterval_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers with nowal, withstore, small ckpt interval are started, pids: $redis_pids" >> /tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"  >> /tmp/redis_cluster_with_eloqstore.log

    run_tcl_tests all $build_type true true

    # kill redis servers
    for pid in "${redis_pids[@]}"; do
      if [[ -n $pid && -e /proc/$pid ]]; then
        kill $pid
      fi
    done

    wait_until_finished

    echo "stop data store server." >> /tmp/redis_cluster_with_eloqstore.log
    # kill data store server
    stop_and_clean_dss_server $kv_store_type
    echo "finished redis_servers with nowal, withstore, small ckpt interval."  >> /tmp/redis_cluster_with_eloqstore.log
    echo ""  >> /tmp/redis_cluster_with_eloqstore.log

    #
    # Test default ckpt interval
    #
    start_dss_server "127.0.0.1" "9100" $kv_store_type
    echo "Data store server is ready!"  >> /tmp/redis_cluster_with_eloqstore.log

    rm -rf /tmp/redis_server_data_0/*
    rm -rf /tmp/redis_server_data_1/*
    rm -rf /tmp/redis_server_data_2/*
    echo "bootstrap before start cluster to avoid contention" >> /tmp/redis_cluster_with_eloqstore.log
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
      --eloq_dss_peer_node=${dss_peer_node} \
      --bootstrap \
      >/tmp/redis_server_multi_node_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    echo "bootstrap nowal, withstore is started, pid: $!"  >> /tmp/redis_cluster_with_eloqstore.log
    # wait for bootstrap to finish
    sleep 20

    redis_pids=()
    local index=0
    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >> /tmp/redis_cluster_with_eloqstore.log
      ${eloqkv_bin_path} \
        --port=$port \
        --core_number=2 \
        --enable_wal=false \
        --enable_data_store=true \
        --log_dir="/tmp/redis_server_logs_$index" \
        --eloq_data_path="/tmp/redis_server_data_$index" \
        --node_memory_limit_mb=${node_memory_limit_mb} \
        --ip_port_list=127.0.0.1:6379,127.0.0.1:7379,127.0.0.1:8379 \
        --txlog_group_replica_num=3 \
        --eloq_dss_peer_node=${dss_peer_node} \
        --maxclients=1000000 \
        --logtostderr=true \
        >/tmp/redis_server_multi_node_nowal_withstore_defaultckptinterval_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers nowal, withstore, default ckpt interval are started, pids: $redis_pids"  >> /tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!"  >> /tmp/redis_cluster_with_eloqstore.log

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
    # kill data store server
    stop_and_clean_dss_server $kv_store_type
    echo "finished redis_servers nowal, withstore, default ckpt interval." >> /tmp/redis_cluster_with_eloqstore.log
    echo ""  >> /tmp/redis_cluster_with_eloqstore.log

    #
    # Test log replay
    #
    echo "starting log service" >> /tmp/redis_cluster_with_eloqstore.log
    local log_service_ip_port="127.0.0.1:9000"

    rm -rf /tmp/log_data
    /home/$current_user/workspace/eloqkv/install/bin/launch_sv \
      -conf=$log_service_ip_port \
      -node_id=0 \
      -storage_path="/tmp/log_data" \
      >/tmp/redis_log_service.log 2>&1 \
      &

    local log_service_pid=$!
    echo "log_service is started, pid: $log_service_pid"
    echo "log_service is started, pid: $log_service_pid" >> /tmp/redis_cluster_with_eloqstore.log
    # wait for log service to be ready
    sleep 10

    start_dss_server "127.0.0.1" "9100" $kv_store_type
    echo "Data store server is ready!"  >> /tmp/redis_cluster_with_eloqstore.log

    rm -rf /tmp/redis_server_data_0/*
    rm -rf /tmp/redis_server_data_1/*
    rm -rf /tmp/redis_server_data_2/*
    echo "bootstrap before start cluster to avoid contention" >> /tmp/redis_cluster_with_eloqstore.log
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
      --txlog_service_list=$log_service_ip_port \
      --txlog_group_replica_num=3 \
      --eloq_dss_peer_node=${dss_peer_node} \
      --bootstrap \
      >/tmp/redis_server_multi_node_withwal_withstore_bootstrap.log 2>&1 \
      &

    echo "bootstrap is started, pid: $!"
    echo "bootstrap is started, pid: $!" >> /tmp/redis_cluster_with_eloqstore.log
    # wait for bootstrap to finish
    sleep 20

    echo "starting redis servers before log replay" >> /tmp/redis_cluster_with_eloqstore.log
    local redis_pids=()
    local index=0

    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >> /tmp/redis_cluster_with_eloqstore.log
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
        --txlog_service_list=$log_service_ip_port \
        --txlog_group_replica_num=3 \
        --eloq_dss_peer_node=${dss_peer_node} \
        >/tmp/redis_server_multi_node_withwal_withstore_beforelogreplay_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers with wal, with datastore before log replay are started, pids: $redis_pids" >> /tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >> /tmp/redis_cluster_with_eloqstore.log

    run_tcl_tests all $build_type true

    echo "Running log replay test for Debug build: " >> /tmp/redis_cluster_with_eloqstore.log

    local python_test_file="${eloqkv_base_path}/tests/unit/$current_user/log_replay_test/log_replay_test.py"
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
    echo "finished redis_servers nowal, withstore before log replay." >> /tmp/redis_cluster_with_eloqstore.log
    echo ""  >> /tmp/redis_cluster_with_eloqstore.log

    # run redis instances again on different ports
    echo "starting redis servers after log replay" >> /tmp/redis_cluster_with_eloqstore.log
    redis_pids=()
    local index=0
    for port in "${ports[@]}"; do
      echo "redirecting output to /tmp/ to prevent ci pipeline crash for $index node." >> /tmp/redis_cluster_with_eloqstore.log
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
        --txlog_service_list=$log_service_ip_port \
        --txlog_group_replica_num=3 \
        --eloq_dss_peer_node=${dss_peer_node} \
        >/tmp/redis_server_multi_node_withwal_withstore_afterlogreplay_$index.log 2>&1 \
        &
      redis_pids+=($!)
      index=$((index + 1))
    done
    echo "redis_servers with wal, with datastore after log replay are started, pids: $redis_pids" >> /tmp/redis_cluster_with_eloqstore.log

    # Wait for Redis server to be ready
    wait_until_ready
    echo "Redis server is ready!" >> /tmp/redis_cluster_with_eloqstore.log

    python3 $python_test_file --verify

    # wait for verify to finish
    sleep 10

    file1="database_snapshot_before_replay.json"  # First JSON file
    file2="database_snapshot_after_replay.json"  # Second JSON file

    if [[ -z "$file1" || -z "$file2" ]]; then
        echo "ERROR: database_snapshot_before_replay.json or database_snapshot_after_replay.json not generated"
        exit 1
    fi

    # Sort JSON content and compare using diff
    if diff <(jq -S . "$file1") <(jq -S . "$file2") &> /dev/null; then
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

    echo "stop data store server." >> /tmp/redis_cluster_with_eloqstore.log
    # kill data store server
    stop_and_clean_dss_server $kv_store_type
    echo "finished redis_servers nowal, withstore after log replay." >> /tmp/redis_cluster_with_eloqstore.log

  fi
}


function run_eloq_test(){
  local build_type=$1
  local kv_store_type=$2

  if [[ "$build_type" != "Debug" ]]; then
    echo "Not Debug build type, skip run_eloq_test."
    return 0
  fi

  # Disable grpc fork protection which might blocks forever.
  # https://github.com/grpc/grpc/blob/master/doc/fork_support.md
  export GRPC_ENABLE_FORK_SUPPORT=0

  local eloqkv_install_path="/home/$current_user/workspace/eloqkv/install"

  if [ ! -d "/home/$current_user/workspace/eloq_test/" ]; then
    echo "/home/$current_user/workspace/eloq_test/ not exists, exit !!!"
  fi

  cd /home/$current_user/workspace/eloq_test
  ./setup

  if [ -d "/home/$current_user/workspace/eloq_test/runtime" ]; then
    rm -rf /home/$current_user/workspace/eloq_test/runtime/*
  else
    mkdir /home/$current_user/workspace/eloq_test/runtime
  fi

  # generate cassandra keyspace name.
  local timestamp=$(($(date +%s%N)/1000000))
  local keyspace_name="redis_test_${timestamp}"

  if [[ $kv_store_type = "CASSANDRA" ]]; then
    echo "cassandra keyspace name is, ${keyspace_name}"
    # drop cassandra keyspace
    /home/$current_user/workspace/apache-cassandra-4.0.6/bin/cqlsh $CASS_HOST -e "DROP KEYSPACE IF EXISTS $keyspace_name;"

    sed -i "s/eloq_keyspace_name.*=.\+/eloq_keyspace_name=${keyspace_name}/g" ./storage.cnf
    sed -i "s/eloq_cass_hosts.*=.\+/eloq_cass_hosts=${CASS_HOST}/g" ./storage.cnf

    sed -i "s/cass_keyspace=.\+/cass_keyspace=${keyspace_name}/g" ./bootstrap_cnf/*_cass.cnf
    sed -i "s/cass_hosts.*=.\+/cass_hosts=${CASS_HOST}/g" ./bootstrap_cnf/*_cass.cnf

    # run cluster scale tests.
    python3 redis_test/single_test/cluster_scale_test.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
    python3 redis_test/single_test/cluster_rolling_upgrade.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
    python3 redis_test/multi_test/cluster_scale_test.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
    python3 redis_test/multi_test/cluster_rolling_upgrade.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
    rm -rf runtime/*

    # run log service scale tests.
    # python3 redis_test/log_service_test/log_service_scale_test.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
    # rm -rf runtime/*


    # run standby tests.
#    python3 redis_test/standby_test/test_without_kv.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
#    rm -rf runtime/*
#    sleep 2
    # python3 redis_test/standby_test/test_with_kv.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
    # rm -rf runtime/*
    # sleep 1
    # python3 redis_test/standby_test/test_with_wal_and_cass.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
    # rm -rf runtime/*
    # sleep 1
    # python3 redis_test/standby_test/test_with_failover.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}

    # # run ttl tests
    # python3 redis_test/ttl_test/ttl_test_with_mem.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
    # python3 redis_test/ttl_test/ttl_test_with_kv.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}
    # python3 redis_test/ttl_test/ttl_test_with_wal.py --dbtype redis --storage cassandra --install_path ${eloqkv_install_path}

#  elif [[ $kv_store_type = "ROCKSDB" ]]; then
#    python3 redis_test/standby_test/test_without_kv.py --dbtype redis --storage rocksdb --install_path ${eloqkv_install_path}
#    rm -rf runtime/*
#    sleep 1
    # python3 redis_test/standby_test/test_with_kv.py --dbtype redis --storage rocksdb --install_path ${eloqkv_install_path}
    # rm -rf runtime/*
    # sleep 1
    # python3 redis_test/standby_test/test_with_failover.py --dbtype redis --storage rocksdb --install_path ${eloqkv_install_path}

    # # run ttl tests
    # python3 redis_test/ttl_test/ttl_test_with_mem.py --dbtype redis --storage rocksdb --install_path ${eloqkv_install_path}
    # python3 redis_test/ttl_test/ttl_test_with_kv.py --dbtype redis --storage rocksdb --install_path ${eloqkv_install_path}
    # python3 redis_test/ttl_test/ttl_test_with_wal.py --dbtype redis --storage rocksdb --install_path ${eloqkv_install_path}


  elif [[ $kv_store_type = "DYNAMODB" ]]; then
    echo "dynamodb keyspace name is, ${keyspace_name}"

    local dynamodb_endpoint=${DYNAMO_ENDPOINT}
    local dynamodb_region=$(sed 's/\//\\\//g' <<< ${AWS_DEFAULT_REGION})
    local aws_access_key_id=$(sed 's/\//\\\//g' <<< ${AWS_ACCESS_KEY_ID})
    local aws_secret_key=$(sed 's/\//\\\//g' <<< ${AWS_SECRET_ACCESS_KEY})
    local dynamo_endpoint_escape=$(sed 's/\//\\\//g' <<< ${DYNAMO_ENDPOINT})

    sed -i "s/eloq_keyspace_name.*=.\+/eloq_keyspace_name=${keyspace_name}/g" ./storage.cnf
    sed -i "s/eloq_dynamodb_endpoint.*=.\+/eloq_dynamodb_endpoint=${dynamo_endpoint_escape}/g" ./storage.cnf
    sed -i "s/eloq_dynamodb_region.*=.\+/eloq_dynamodb_region=${dynamodb_region}/g" ./storage.cnf
    sed -i "s/eloq_aws_access_key_id.*=.\+/eloq_aws_access_key_id=${aws_access_key_id}/g" ./storage.cnf
    sed -i "s/eloq_aws_secret_key.*=.\+/eloq_aws_secret_key=${aws_secret_key}/g" ./storage.cnf

    sed -i "s/dynamodb_keyspace=.\+/dynamodb_keyspace=${keyspace_name}/g" ./bootstrap_cnf/*_dynamo.cnf
    sed -i "s/dynamodb_endpoint.*=.\+/dynamodb_endpoint=${dynamo_endpoint_escape}/g" ./bootstrap_cnf/*_dynamo.cnf
    sed -i "s/dynamodb_region.*=.\+/dynamodb_region=${dynamodb_region}/g" ./bootstrap_cnf/*_dynamo.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${aws_access_key_id}/g" ./bootstrap_cnf/*_dynamo.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${aws_secret_key}/g" ./bootstrap_cnf/*_dynamo.cnf

    # run cluster scale tests.
    python3 redis_test/single_test/cluster_scale_test.py --dbtype redis --storage dynamo --install_path ${eloqkv_install_path}
    python3 redis_test/multi_test/cluster_scale_test.py --dbtype redis --storage dynamo --install_path ${eloqkv_install_path}
    rm -rf runtime/*
    # run standby tests.
#    python3 redis_test/standby_test/test_without_kv.py --dbtype redis --storage dynamo --install_path ${eloqkv_install_path}
#    rm -rf runtime/*
#    sleep 2
    # python3 redis_test/standby_test/test_with_kv.py --dbtype redis --storage dynamo --install_path ${eloqkv_install_path}
    # rm -rf runtime/*
    # sleep 1
    # python3 redis_test/standby_test/test_with_wal_and_cass.py --dbtype redis --storage dynamo --install_path ${eloqkv_install_path}
    # rm -rf runtime/*
    # sleep 1
    # python3 redis_test/standby_test/test_with_failover.py --dbtype redis --storage dynamo --install_path ${eloqkv_install_path}


  elif [[ $kv_store_type = "ELOQDSS_ROCKSDB_CLOUD_S3" ]]; then

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

    sed -i "s/rocksdb_cloud_s3_endpoint_url.*=.\+/rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g"  ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/txlog_rocksdb_cloud_s3_endpoint_url.*=.\+/txlog_rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g"  ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${rocksdb_cloud_aws_access_key_id}/g"  ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${rocksdb_cloud_aws_secret_access_key}/g"  ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/rocksdb_cloud_bucket_name.*=.\+/rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g"  ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/txlog_rocksdb_cloud_bucket_name.*=.\+/txlog_rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g"  ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/rocksdb_cloud_object_path.*=.\+/rocksdb_cloud_object_path=${rocksdb_cloud_object_path}/g"  ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf
    sed -i "s/txlog_rocksdb_cloud_object_path.*=.\+/txlog_rocksdb_cloud_object_path=${txlog_rocksdb_cloud_object_path}/g"  ./bootstrap_cnf/*_rocksdb_cloud_s3.cnf

    sed -i "s/rocksdb_cloud_s3_endpoint_url.*=.\+/rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g"  ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/txlog_rocksdb_cloud_s3_endpoint_url.*=.\+/txlog_rocksdb_cloud_s3_endpoint_url=${rocksdb_cloud_s3_endpoint_url_escape}/g"  ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/aws_access_key_id.*=.\+/aws_access_key_id=${rocksdb_cloud_aws_access_key_id}/g"  ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/aws_secret_key.*=.\+/aws_secret_key=${rocksdb_cloud_aws_secret_access_key}/g"  ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/rocksdb_cloud_bucket_name.*=.\+/rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g"  ./bootstrap_cnf/eloqdss_server.cnf
    sed -i "s/txlog_rocksdb_cloud_bucket_name.*=.\+/txlog_rocksdb_cloud_bucket_name=${rocksdb_cloud_bucket_name}/g"  ./bootstrap_cnf/eloqdss_server.cnf

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
    python3 run_tests.py --dbtype redis --group standby --storage eloqdss-rocksdb-cloud-s3 --install_path ${eloqkv_install_path}
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
    # run single/multi test
    rm -rf runtime/*
    python3 run_tests.py --dbtype redis --group single --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}

    python3 redis_test/multi_test/smoke_test.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}

    # disable unstable multi test
    # python3 redis_test/multi_test/cluster_rolling_upgrade.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}
    # python3 redis_test/multi_test/cluster_scale_test.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}

    # run log service scale test
    rm -rf runtime/*
    python3 redis_test/log_service_test/log_service_scale_test.py --dbtype redis --storage eloqdss-eloqstore --install_path ${eloqkv_install_path}

    # run standby test
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
