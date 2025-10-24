#!/bin/bash
set -exo pipefail

source "$(dirname "$0")/common.sh"


CWDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ls
export WORKSPACE=$PWD

MINIO_ENDPOINT=${1:?usage: $0 minio_endpoint minio_access_key minio_secret_key git_ssh_key}
MINIO_ACCESS_KEY=${2:?usage: $0 minio_endpoint minio_access_key minio_secret_key git_ssh_key}
MINIO_SECRET_KEY=${3:?usage: $0 minio_endpoint minio_access_key minio_secret_key git_ssh_key}
GIT_SSH_KEY=${4:?usage: $0 minio_endpoint minio_access_key minio_secret_key git_ssh_key}

mkdir -p ~/.ssh
echo "$GIT_SSH_KEY" > ~/.ssh/id_rsa
chmod 600 ~/.ssh/id_rsa
ssh-keyscan github.com >> ~/.ssh/known_hosts

MINIO_ENDPOINT_ESCAPE=$(sed 's/\//\\\//g' <<< $MINIO_ENDPOINT)
export ROCKSDB_CLOUD_S3_ENDPOINT=${MINIO_ENDPOINT}
export ROCKSDB_CLOUD_S3_ENDPOINT_ESCAPE=${MINIO_ENDPOINT_ESCAPE}
export ROCKSDB_CLOUD_AWS_ACCESS_KEY_ID=${MINIO_ACCESS_KEY}
export ROCKSDB_CLOUD_AWS_SECRET_ACCESS_KEY=${MINIO_SECRET_KEY}
timestamp=$(($(date +%s%N) / 1000000))
ROCKSDB_CLOUD_BUCKET_NAME="test-${timestamp}"
ROCKSDB_CLOUD_OBJECT_PATH="test-db"
export ROCKSDB_CLOUD_BUCKET_NAME=${ROCKSDB_CLOUD_BUCKET_NAME}
export ROCKSDB_CLOUD_OBJECT_PATH=${ROCKSDB_CLOUD_OBJECT_PATH}


cd $WORKSPACE
whoami
pwd
ls
current_user=$(whoami)
sudo chown -R $current_user $PWD

ulimit -c unlimited
echo '/tmp/core.%t.%e.%p' | sudo tee /proc/sys/kernel/core_pattern
# make coredump dir writable.
if [ ! -d "/var/crash" ]; then sudo mkdir -p /var/crash; fi
sudo chmod 777 /var/crash

sudo chown -R $current_user /home/$current_user/workspace
cd /home/$current_user/workspace
ln -s $WORKSPACE/redis_pr eloqkv
ln -s $WORKSPACE/eloq_test_src eloq_test

cd eloqkv
git submodule sync
git submodule update --init --recursive
pr_branch_name=$(cat .git/resource/metadata.json | jq -r '.[] | select(.name=="head_name") | .value')

ln -s $WORKSPACE/logservice_src data_substrate/eloq_log_service
cd data_substrate/eloq_log_service
if [ -n "$pr_branch_name" ] && git ls-remote --exit-code --heads origin "$pr_branch_name" > /dev/null; then
  git fetch origin "${pr_branch_name}:refs/remotes/origin/${pr_branch_name}"
  git checkout -b ${pr_branch_name} origin/${pr_branch_name}
  git submodule update --init --recursive
fi

cd /home/$current_user/workspace/eloqkv/data_substrate/tx_service

ln -s $WORKSPACE/raft_host_manager_src raft_host_manager
cd raft_host_manager
if [ -n "$pr_branch_name" ] && git ls-remote --exit-code --heads origin "$pr_branch_name" > /dev/null; then
  git fetch origin "${pr_branch_name}:refs/remotes/origin/${pr_branch_name}"
  git checkout -b ${pr_branch_name} origin/${pr_branch_name}
  git submodule update --init --recursive
fi
cd ..

cd /home/$current_user/workspace/eloqkv

cmake_version=$(cmake --version 2>&1)
if [[ $? -eq 0 ]]; then
  echo "cmake version: $cmake_version"
else
  echo "fail to get cmake version"
fi

sudo apt-get update
sudo apt install software-properties-common -y
sudo add-apt-repository ppa:deadsnakes/ppa -y
sudo apt update
sudo apt install python3.8 python3.8-venv python3.8-dev -y

# todo: move these code to docker-image
sudo apt install openssh-server -y
sudo service ssh start
# disable ask when do ssh
sudo sed -i "s/#\s*StrictHostKeyChecking ask/    StrictHostKeyChecking no/g" /etc/ssh/ssh_config

python3.8 -m venv my_env
source my_env/bin/activate
pip install -r /home/$current_user/workspace/eloqkv/tests/unit/mono/log_replay_test/requirements.txt
deactivate

build_types=("Debug")
# kv_store_types=("CASSANDRA" "ROCKSDB")
kv_store_types=("ELOQDSS_ROCKSDB_CLOUD_S3" "ROCKSDB")


for bt in "${build_types[@]}"; do
  for kst in "${kv_store_types[@]}"; do
    rm -rf /home/$current_user/workspace/eloqkv/eloq_data
    run_build_ent $bt $kst

    source my_env/bin/activate
    run_eloq_test $bt $kst
    run_eloqkv_tests $bt $kst
    run_eloqkv_cluster_tests $bt $kst
    deactivate

  done
done

# # test ttl
# cd /home/$current_user/workspace/eloqkv
# source my_env/bin/activate
# pip install -r /home/$current_user/workspace/eloq_test/py_requirements.txt
# rm -rf /home/$current_user/workspace/eloqkv/eloq_data
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
