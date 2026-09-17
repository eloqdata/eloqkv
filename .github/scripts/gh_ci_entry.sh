#!/bin/bash
set -Eexo pipefail

ulimit -n
ulimit -l

# Enable core dumps and route them to a known, retrievable location so that
# dump_ci_failure_logs/dump_core_backtraces can symbolize crashes. The ent-ci
# container runs --privileged, so writing the host core_pattern works here.
ulimit -c unlimited
echo '/tmp/core.%e.%p.%t' | tee /proc/sys/kernel/core_pattern >/dev/null 2>&1 \
  || echo "warning: could not set core_pattern (need privileged); cores may be intercepted by apport"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

ls
export WORKSPACE=$PWD

USAGE="usage: $0 s3_endpoint s3_access_key s3_secret_key kv_store_type build|test|cluster"
S3_ENDPOINT=${1:?$USAGE}
S3_ACCESS_KEY=${2:?$USAGE}
S3_SECRET_KEY=${3:?$USAGE}
KV_STORE_TYPE=${4:?$USAGE}
CI_PHASE=${5:?$USAGE}

case "$CI_PHASE" in
  build | test | cluster) ;;
  *) echo "$USAGE" >&2; exit 1 ;;
esac

BUILD_TYPE=${BUILD_TYPE:?BUILD_TYPE env var not set}
CI_MODE=${CI_MODE:-pr}             # "pr" or "main"
ELOQ_TEST_BRANCH=${ELOQ_TEST_BRANCH:-github-action}

# The main repo is now checked out to GITHUB_WORKSPACE/eloqkv.
# All auxiliary repos are cloned alongside it under GITHUB_WORKSPACE/.
export ELOQKV_BASE_PATH="${GITHUB_WORKSPACE}/eloqkv"
export ELOQ_TEST_PATH="${GITHUB_WORKSPACE}/eloq_test_src"

# Use an EXIT trap rather than ERR: the test helpers call `exit 1` directly on
# failure, which does not trigger ERR. EXIT catches both command failures (via
# set -e) and explicit exits. Guard on rc so success is not treated as failure.
trap 'rc=$?; failed_command=$BASH_COMMAND; set +x; if [ "$rc" -ne 0 ]; then dump_ci_failure_logs "$rc" "$failed_command"; fi; stop_rustfs; exit "$rc"' EXIT

# Compute txlog_log_state from kv_store_type (same as pr.ent.bash)
if [ "$KV_STORE_TYPE" == "ELOQDSS_ROCKSDB_CLOUD_S3" ]; then
  txlog_log_state="ROCKSDB_CLOUD_S3"
elif [ "$KV_STORE_TYPE" == "ELOQDSS_ELOQSTORE" ]; then
  txlog_log_state="ROCKSDB_CLOUD_S3"
elif [ "$KV_STORE_TYPE" == "ROCKSDB" ]; then
  txlog_log_state="ROCKSDB"
fi

echo "CI_MODE=$CI_MODE BUILD_TYPE=$BUILD_TYPE KV_STORE_TYPE=$KV_STORE_TYPE txlog_log_state=$txlog_log_state"

# --- S3 env exports ---
S3_ENDPOINT_ESCAPE=$(sed 's/\//\\\//g' <<< $S3_ENDPOINT)
export ROCKSDB_CLOUD_S3_ENDPOINT=${S3_ENDPOINT}
export ROCKSDB_CLOUD_S3_ENDPOINT_ESCAPE=${S3_ENDPOINT_ESCAPE}
export ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID=${S3_ACCESS_KEY}
export ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY=${S3_SECRET_KEY}
export ROCKSDB_CLOUD_BUCKET_PREFIX="eloqkv-"
export ROCKSDB_CLOUD_BUCKET_NAME="test"
export ELOQSTORE_BUCKET_NAME="eloqkv-eloqstore-test"
export ROCKSDB_CLOUD_OBJECT_PATH="dss"
export TXLOG_ROCKSDB_CLOUD_OBJECT_PATH="txlog"

# The build phase does not touch object storage. The existing test venv supplies
# AWS CLI for bucket cleanup, so no MinIO server/client downloads are needed.
if [ "$CI_PHASE" != "build" ]; then
  "${ELOQ_TEST_VENV:-/opt/eloq/test-venv}/bin/aws" --version
  start_rustfs "$S3_ENDPOINT" "$S3_ACCESS_KEY" "$S3_SECRET_KEY"
fi

# --- Workspace setup ---
# All repos live under GITHUB_WORKSPACE.
# Symlink auxiliary repos into their expected locations within the main repo.
cd ${ELOQKV_BASE_PATH}

# --- Submodule init ---
git submodule sync
git submodule update --init --recursive

# --- eloq_test branch setup ---
cd ${ELOQ_TEST_PATH}
git fetch origin "${ELOQ_TEST_BRANCH}:refs/remotes/origin/${ELOQ_TEST_BRANCH}"
git checkout -B "${ELOQ_TEST_BRANCH}" "origin/${ELOQ_TEST_BRANCH}"
git submodule update --init --recursive

# eloq_log_service and raft_host_manager now live in-tree within the
# data_substrate submodule, so they are populated by the submodule update above;
# no separate clone or symlink is needed.

# --- CMake version check ---
cd ${ELOQKV_BASE_PATH}

cmake_version=$(cmake --version 2>&1)
if [[ $? -eq 0 ]]; then
  echo "cmake version: $cmake_version"
else
  echo "fail to get cmake version"
fi

# --- OpenSSH + Python 3.8 test venv come pre-baked in the ubuntu-dev image ---
# (python3.8 + log_replay_test/requirements.txt live in $LOG_REPLAY_VENV; see
# eloq-docker/ubuntu-dev/Dockerfile). No Python packages are installed here.
service ssh start
sed -i "s/#\s*StrictHostKeyChecking ask/    StrictHostKeyChecking no/g" /etc/ssh/ssh_config

VENV="${LOG_REPLAY_VENV:-/opt/eloq/log-replay-venv}"

# --- Run build and/or tests for single (build_type, kv_store_type) ---
if [ "$CI_PHASE" = "build" ]; then
  rm -rf ${ELOQKV_BASE_PATH}/eloq_data
  run_build_ent $BUILD_TYPE $KV_STORE_TYPE $txlog_log_state
fi

if [ "$CI_PHASE" = "test" ]; then
  source "$VENV/bin/activate"
  run_eloq_test $BUILD_TYPE $KV_STORE_TYPE
  run_eloqkv_tests $BUILD_TYPE $KV_STORE_TYPE
  deactivate
fi

if [ "$CI_PHASE" = "cluster" ]; then
  source "$VENV/bin/activate"
  run_eloqkv_cluster_tests $BUILD_TYPE $KV_STORE_TYPE
  deactivate
fi

echo "CI $CI_PHASE completed successfully for $CI_MODE BUILD_TYPE=$BUILD_TYPE KV_STORE_TYPE=$KV_STORE_TYPE"
