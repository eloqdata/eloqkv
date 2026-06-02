#!/bin/bash
set -exo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

ls
export WORKSPACE=$PWD

MINIO_ENDPOINT=${1:?usage: $0 minio_endpoint minio_access_key minio_secret_key kv_store_type [git_ssh_key]}
MINIO_ACCESS_KEY=${2:?usage: $0 minio_endpoint minio_access_key minio_secret_key kv_store_type [git_ssh_key]}
MINIO_SECRET_KEY=${3:?usage: $0 minio_endpoint minio_access_key minio_secret_key kv_store_type [git_ssh_key]}
KV_STORE_TYPE=${4:?usage: $0 minio_endpoint minio_access_key minio_secret_key kv_store_type [git_ssh_key]}
GIT_SSH_KEY=${5:-}

BUILD_TYPE=${BUILD_TYPE:?BUILD_TYPE env var not set}
CI_MODE=${CI_MODE:-pr}             # "pr" or "main"
PR_BRANCH_NAME=${PR_BRANCH_NAME:-}

# Determine checkout dir based on CI mode
if [ "$CI_MODE" == "main" ]; then
  CHECKOUT_DIR="redis_src"
else
  CHECKOUT_DIR="redis_pr"
fi

# Compute txlog_log_state from kv_store_type (same as pr.ent.bash)
if [ "$KV_STORE_TYPE" == "ELOQDSS_ROCKSDB_CLOUD_S3" ]; then
  txlog_log_state="ROCKSDB_CLOUD_S3"
elif [ "$KV_STORE_TYPE" == "ELOQDSS_ELOQSTORE" ]; then
  txlog_log_state="ROCKSDB_CLOUD_S3"
elif [ "$KV_STORE_TYPE" == "ROCKSDB" ]; then
  txlog_log_state="ROCKSDB"
fi

echo "CI_MODE=$CI_MODE BUILD_TYPE=$BUILD_TYPE KV_STORE_TYPE=$KV_STORE_TYPE txlog_log_state=$txlog_log_state"

# --- SSH key setup (only for PR mode) ---
if [ -n "$GIT_SSH_KEY" ]; then
  mkdir -p ~/.ssh
  echo "$GIT_SSH_KEY" > ~/.ssh/id_rsa
  chmod 600 ~/.ssh/id_rsa
  ssh-keyscan github.com >> ~/.ssh/known_hosts
fi

# --- Minio env exports ---
MINIO_ENDPOINT_ESCAPE=$(sed 's/\//\\\//g' <<< $MINIO_ENDPOINT)
export ROCKSDB_CLOUD_S3_ENDPOINT=${MINIO_ENDPOINT}
export ROCKSDB_CLOUD_S3_ENDPOINT_ESCAPE=${MINIO_ENDPOINT_ESCAPE}
export ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID=${MINIO_ACCESS_KEY}
export ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY=${MINIO_SECRET_KEY}
timestamp=$(($(date +%s%N) / 1000000))
export ROCKSDB_CLOUD_BUCKET_PREFIX="eloqkv-"
export ROCKSDB_CLOUD_BUCKET_NAME="test-${timestamp}"
export ELOQSTORE_BUCKET_NAME="eloqkv-eloqstore-test-${timestamp}"
export ROCKSDB_CLOUD_OBJECT_PATH="dss"
export TXLOG_ROCKSDB_CLOUD_OBJECT_PATH="txlog"

# --- Download & start Minio ---
echo "Downloading and starting Minio..."
wget -q https://dl.min.io/server/minio/release/linux-amd64/minio
chmod +x minio
mkdir -p /tmp/minio_data
MINIO_ROOT_USER=$MINIO_ACCESS_KEY MINIO_ROOT_PASSWORD=$MINIO_SECRET_KEY \
  ./minio server /tmp/minio_data --address :9000 --console-address :9001 > /tmp/minio.log 2>&1 &
MINIO_PID=$!

echo "Waiting for Minio to be ready..."
for i in $(seq 1 30); do
  if curl -sf http://localhost:9000/minio/health/live > /dev/null 2>&1; then
    echo "Minio is ready."
    break
  fi
  if ! kill -0 $MINIO_PID 2>/dev/null; then
    echo "Minio process died. Log:"
    cat /tmp/minio.log
    exit 1
  fi
  sleep 1
done

if ! curl -sf http://localhost:9000/minio/health/live > /dev/null 2>&1; then
  echo "Minio failed to start after 30s. Log:"
  cat /tmp/minio.log
  exit 1
fi

# --- Setup mc (MinIO Client) ---
echo "Downloading and configuring mc..."
wget -q https://dl.min.io/client/mc/release/linux-amd64/mc
chmod +x mc
mv mc /usr/local/bin/mc
mc alias set local http://localhost:9000 $MINIO_ACCESS_KEY $MINIO_SECRET_KEY

# --- Workspace setup ---
cd $WORKSPACE
whoami
pwd
ls
current_user=$(whoami)
chown -R $current_user $PWD

ulimit -c unlimited
echo '/tmp/core.%t.%e.%p' | tee /proc/sys/kernel/core_pattern
if [ ! -d "/var/crash" ]; then mkdir -p /var/crash; fi
chmod 777 /var/crash

mkdir -p /home/$current_user/workspace
chown -R $current_user /home/$current_user/workspace
cd /home/$current_user/workspace
ln -sf $WORKSPACE/$CHECKOUT_DIR eloqkv
ln -sf $WORKSPACE/eloq_test_src eloq_test

# --- Submodule init ---
cd eloqkv
git submodule sync
git submodule update --init --recursive
cd ..

# --- PR branch matching for eloq_test (PR mode only) ---
cd eloq_test
if [ -n "$PR_BRANCH_NAME" ] && git ls-remote --exit-code --heads origin "$PR_BRANCH_NAME" > /dev/null 2>&1; then
  git fetch origin "${PR_BRANCH_NAME}:refs/remotes/origin/${PR_BRANCH_NAME}"
  git checkout -b ${PR_BRANCH_NAME} origin/${PR_BRANCH_NAME}
  git submodule update --init --recursive
fi
cd ..

# --- Setup log service symlink and branch matching ---
cd eloqkv
ln -sf $WORKSPACE/logservice_src data_substrate/eloq_log_service
cd data_substrate/eloq_log_service
if [ -n "$PR_BRANCH_NAME" ] && git ls-remote --exit-code --heads origin "$PR_BRANCH_NAME" > /dev/null 2>&1; then
  git fetch origin "${PR_BRANCH_NAME}:refs/remotes/origin/${PR_BRANCH_NAME}"
  git checkout -b ${PR_BRANCH_NAME} origin/${PR_BRANCH_NAME}
  git submodule update --init --recursive
fi

# --- Setup raft_host_manager symlink and branch matching ---
cd /home/$current_user/workspace/eloqkv/data_substrate/tx_service
ln -sf $WORKSPACE/raft_host_manager_src raft_host_manager
cd raft_host_manager
if [ -n "$PR_BRANCH_NAME" ] && git ls-remote --exit-code --heads origin "$PR_BRANCH_NAME" > /dev/null 2>&1; then
  git fetch origin "${PR_BRANCH_NAME}:refs/remotes/origin/${PR_BRANCH_NAME}"
  git checkout -b ${PR_BRANCH_NAME} origin/${PR_BRANCH_NAME}
  git submodule update --init --recursive
fi
cd ..

# --- CMake version check ---
cd /home/$current_user/workspace/eloqkv

cmake_version=$(cmake --version 2>&1)
if [[ $? -eq 0 ]]; then
  echo "cmake version: $cmake_version"
else
  echo "fail to get cmake version"
fi

# --- Install Python 3.8 + OpenSSH server ---
apt-get update
apt-get install software-properties-common -y
add-apt-repository ppa:deadsnakes/ppa -y
apt-get update
apt-get install python3.8 python3.8-venv python3.8-dev -y

apt-get install openssh-server -y
service ssh start
sed -i "s/#\s*StrictHostKeyChecking ask/    StrictHostKeyChecking no/g" /etc/ssh/ssh_config

python3.8 -m venv my_env
source my_env/bin/activate
pip install -r /home/$current_user/workspace/eloqkv/tests/unit/eloq/log_replay_test/requirements.txt
deactivate

# --- Run build + tests for single (build_type, kv_store_type) ---
rm -rf /home/$current_user/workspace/eloqkv/eloq_data

run_build_ent $BUILD_TYPE $KV_STORE_TYPE $txlog_log_state

source my_env/bin/activate
run_eloq_test $BUILD_TYPE $KV_STORE_TYPE
run_eloqkv_tests $BUILD_TYPE $KV_STORE_TYPE
run_eloqkv_cluster_tests $BUILD_TYPE $KV_STORE_TYPE
deactivate

# --- Cleanup Minio ---
echo "Stopping Minio (pid $MINIO_PID)..."
kill $MINIO_PID 2>/dev/null || true
wait $MINIO_PID 2>/dev/null || true
rm -rf /tmp/minio_data
rm -f ./minio

echo "CI completed successfully for $CI_MODE BUILD_TYPE=$BUILD_TYPE KV_STORE_TYPE=$KV_STORE_TYPE"
