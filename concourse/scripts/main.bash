#!/bin/bash
set -exo pipefail

source "$(dirname "$0")/common.sh"

CASS_HOST=${1:?usage: $0 cass_host}

CWDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ls
export WORKSPACE=$PWD
export CASS_HOST=$CASS_HOST

MINIO_ENDPOINT=${2:?usage: $0 cass_host minio_endpoint minio_access_key minio_secret_key}
MINIO_ACCESS_KEY=${3:?usage: $0 cass_host minio_endpoint minio_access_key minio_secret_key}
MINIO_SECRET_KEY=${4:?usage: $0 cass_host minio_endpoint minio_access_key minio_secret_key}

MINIO_ENDPOINT_ESCAPE=$(sed 's/\//\\\//g' <<< $MINIO_ENDPOINT)
export ROCKSDB_CLOUD_S3_ENDPOINT=${MINIO_ENDPOINT}
export ROCKSDB_CLOUD_S3_ENDPOINT_ESCAPE=${MINIO_ENDPOINT_ESCAPE}
export ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID=${MINIO_ACCESS_KEY}
export ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY=${MINIO_SECRET_KEY}
timestamp=$(($(date +%s%N) / 1000000))
ROCKSDB_CLOUD_BUCKET_NAME="test-${timestamp}"
export ROCKSDB_CLOUD_BUCKET_NAME=${ROCKSDB_CLOUD_BUCKET_NAME}


cd $WORKSPACE
whoami
pwd
ls
sudo chown -R mono $PWD

ulimit -c unlimited
echo '/tmp/core.%t.%e.%p' | sudo tee /proc/sys/kernel/core_pattern

sudo chown -R mono /home/mono/workspace
cd /home/mono/workspace
ln -s $WORKSPACE/redis_src eloqkv
ln -s $WORKSPACE/eloq_test_src eloq_test

cd eloqkv
git submodule sync
git submodule update --init --recursive

ln -s open_log_service log_service

cd /home/mono/workspace/eloqkv/tx_service

git checkout main

cd /home/mono/workspace/eloqkv

cmake_version=$(cmake --version 2>&1)
if [[ $? -eq 0 ]]; then
  echo "cmake version: $cmake_version"
else
  echo "fail to get cmake version"
fi

sudo apt-get update

python3 -m venv my_env
source my_env/bin/activate
pip install -r /home/mono/workspace/eloqkv/tests/unit/mono/log_replay_test/requirements.txt
deactivate

build_types=("Debug")
# kv_store_types=("CASSANDRA" "ROCKSDB")
kv_store_types=("ROCKSDB")


for bt in "${build_types[@]}"; do
  for kst in "${kv_store_types[@]}"; do
    rm -rf /home/mono/workspace/eloqkv/eloq_data
    run_build $bt $kst

    source my_env/bin/activate
    run_eloq_test $bt $kst
    run_eloqkv_tests $bt $kst
    run_eloqkv_cluster_tests $bt $kst
    deactivate

  done
done

# # test ttl
# cd /home/mono/workspace/eloqkv
# source my_env/bin/activate
# pip install -r /home/mono/workspace/eloq_test/py_requirements.txt
# rm -rf /home/mono/workspace/eloqkv/eloq_data
# run_build "Debug" "ROCKSDB"
# #                       testcase enable_wal enable_data_store
# run_eloq_ttl_tests TestsWithMem true true rocksdb
# run_eloq_ttl_tests TestsWithKV true true rocksdb
# run_eloq_ttl_tests TestsWithLog true true rocksdb
# run_eloq_ttl_tests TestsWithMem false true rocksdb
# run_eloq_ttl_tests TestsWithKV false true rocksdb
# run_eloq_ttl_tests TestsWithMem false false rocksdb

# # run_build "Debug" "CASSANDRA"
# # #                       testcase enable_wal enable_data_store
# # run_eloq_ttl_tests TestsWithMem true true cassandra $CASS_HOST
# # run_eloq_ttl_tests TestsWithKV true true cassandra $CASS_HOST
# # run_eloq_ttl_tests TestsWithLog true true cassandra $CASS_HOST
# # run_eloq_ttl_tests TestsWithMem false true cassandra $CASS_HOST
# # run_eloq_ttl_tests TestsWithKV false true cassandra $CASS_HOST
# # run_eloq_ttl_tests TestsWithMem false false cassandra $CASS_HOST
# deactivate
