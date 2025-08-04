

eloqkv_dir="/home/lzx/eloqkv/build"
cass_dir="/home/lzx/apache-cassandra-4.1.1/bin"
mono_src_dir="/home/lzx/eloqkv"
export CASS_HOST=127.0.0.1
echo  ${eloqkv_dir}

ulimit -n

# Function to check if Redis server is ready
function is_redis_ready() {
  if [ $# -gt 0 ]; then
    redis-cli -h 127.0.0.1 -p $1 ping | grep -q "PONG"
  else
    redis-cli -h 127.0.0.1 -p 6379 ping | grep -q "PONG"
  fi
}

function run_eloqkv_standby_tests() {
  local build_type=$1

  # generate cassandra keyspace name.
  local timestamp=$(($(date +%s%N) / 1000000))
  local keyspace_name="redis_test_${timestamp}"
  echo "cassandra keyspace name is, ${keyspace_name}"
  "${cass_dir}"/cqlsh $CASS_HOST -e "DROP KEYSPACE IF EXISTS $keyspace_name;"

  # run redis-6379
  echo "redirecting output to /tmp/ to prevent ci pipeline crash"
  ${eloqkv_dir}/eloqkv \
    --ip=127.0.0.1 \
    --port=6379 \
    --core_number=2 \
    --enable_wal=none \
    --enable_data_store=all \
    --cass_hosts=$CASS_HOST \
    --cass_keyspace=$keyspace_name \
    --maxclients=1000000 \
    --checkpoint_interval=36000 \
    --ip_port_list=127.0.0.1:6379 \
    --standby_ip_port_list="127.0.0.1:7379|127.0.0.1:8379" \
    --logtostderr=true \
    >tmp/redis_server_single_standby_6379.log 2>&1 \
    &

  local redis_pid_6379=$!
  echo "redis_pid_6379:${redis_pid_6379}"

  sleep 5
  # run redis-7379
  echo "redirecting output to /tmp/ to prevent ci pipeline crash"
  ${eloqkv_dir}/eloqkv \
    --ip=127.0.0.1 \
    --port=7379 \
    --core_number=2 \
    --enable_wal=none \
    --enable_data_store=all \
    --cass_hosts=$CASS_HOST \
    --cass_keyspace=$keyspace_name \
    --maxclients=1000000 \
    --checkpoint_interval=36000 \
    --ip_port_list=127.0.0.1:6379 \
    --standby_ip_port_list="127.0.0.1:7379|127.0.0.1:8379" \
    --logtostderr=true \
    >tmp/redis_server_single_standby_7379.log 2>&1 \
    &

  local redis_pid_7379=$!
  echo "redis_pid_7379:${redis_pid_7379}"

  sleep 5
  # run redis-8379
  echo "redirecting output to /tmp/ to prevent ci pipeline crash"
  ${eloqkv_dir}/eloqkv \
    --ip=127.0.0.1 \
    --port=8379 \
    --core_number=2 \
    --enable_wal=none \
    --enable_data_store=all \
    --cass_hosts=$CASS_HOST \
    --cass_keyspace=$keyspace_name \
    --maxclients=1000000 \
    --checkpoint_interval=36000 \
    --ip_port_list=127.0.0.1:6379 \
    --standby_ip_port_list="127.0.0.1:7379|127.0.0.1:8379" \
    --logtostderr=true \
    >tmp/redis_server_single_standby_8379.log 2>&1 \
    &

  local redis_pid_8379=$!
  echo "redis_pid_8379:${redis_pid_8379}"

  
  sleep 5

  # Wait for Redis server to be ready
  until is_redis_ready; do
    echo "Waiting for Redis server 6379 to be ready..."
    sleep 1
  done
  echo "Redis server 6379 is ready!"

  # fail over to standby node test
  if [ $build_type = "Debug" ]; then
    echo "Running failover to standby node test for Debug build: "

    local python_test_file="${mono_src_dir}/tests/unit/mono/log_replay_test/log_replay_test.py"
    python3 $python_test_file --load > tmp/redis_standby_test_load.log 2>&1

    # wait for load to finish
    sleep 10

    # kill redis_server
    if [[ -n $redis_pid_6379 && -e /proc/$redis_pid_6379 ]]; then
      echo "kill redis-6379: $redis_pid_6379"
      kill -9 $redis_pid_6379
    fi

    # wait for kill to finish
    sleep 5

    # Wait for Redis server to be ready
    until is_redis_ready 7379 ; do
      echo "Waiting for Redis server 7379 to be ready..."
      sleep 1
    done
    echo "Redis server 7379 is ready!"

    redis-cli -h 127.0.0.1 -p 7379 get a

    python3 $python_test_file --verify --port 7379

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

  fi

  # kill redis_server
  if [[ -n $redis_pid_7379 && -e /proc/$redis_pid_7379 ]]; then
    kill $redis_pid_7379
  fi
  
  if [[ -n $redis_pid_8379 && -e /proc/$redis_pid_8379 ]]; then
    kill $redis_pid_8379
  fi

  cd /home/$current_user/workspace/eloqkv
  if [ -d "./cc_ng" ]; then
    rm -rf ./cc_ng
  fi
  if [ -d "./tx_log" ]; then
    rm -rf ./tx_log
  fi


  # drop cassandra keyspace
  ${cass_dir}/cqlsh $CASS_HOST -e "DROP KEYSPACE IF EXISTS $keyspace_name;"
}


run_eloqkv_standby_tests "Debug"