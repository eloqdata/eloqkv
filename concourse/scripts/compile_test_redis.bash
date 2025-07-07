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

# make coredump dir writable.
if [ ! -d "/var/crash" ]; then sudo mkdir -p /var/crash; fi
sudo chmod 777 /var/crash

cd $WORKSPACE/redis_pr
pr_branch_name=$(cat .git/resource/metadata.json | jq -r '.[] | select(.name=="head_name") | .value')
sudo chown -R mono /home/mono/workspace
cd /home/mono/workspace/

ln -s $WORKSPACE/redis_src eloqkv
cd eloqkv
set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
redis_branch=$(git branch -a | grep "remotes/origin/${pr_branch_name}" | grep -v grep)
set -e
if [ -n "$redis_branch" ]; then git checkout -b ${pr_branch_name} origin/${pr_branch_name}; fi
git submodule sync
git submodule update --init --recursive

ln -s $WORKSPACE/logservice_src log_service

cd tx_service
set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
tx_branch=$(git branch -a | grep "remotes/origin/${pr_branch_name}" | grep -v grep)
set -e
if [ -n "$tx_branch" ]; then git checkout -b ${pr_branch_name} origin/${pr_branch_name}; fi

cd tx-log-protos
set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
tx_log_protos_branch=`git branch -a| grep "remotes/origin/${pr_branch_name}" | grep -v grep`
set -e
if [ -n "$tx_log_protos_branch" ]; then git checkout -b ${pr_branch_name} origin/${pr_branch_name}; fi
cd ..

cd /home/mono/workspace/eloqkv/log_service
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
set +e
log_branch=$(git branch -a | grep "remotes/origin/${pr_branch_name}" | grep -v grep)
set -e
if [ -n "$log_branch" ]; then git checkout -b ${pr_branch_name} origin/${pr_branch_name}; fi

cd tx-log-protos
set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
tx_log_protos_branch=`git branch -a| grep "remotes/origin/${pr_branch_name}" | grep -v grep`
set -e
if [ -n "$tx_log_protos_branch" ]; then git checkout -b ${pr_branch_name} origin/${pr_branch_name}; fi
cd ..

cd /home/mono/workspace/eloqkv/store_handler
set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
store_handler_branch=$(git branch -a | grep "remotes/origin/${pr_branch_name}" | grep -v grep)
set -e
if [ -n "$store_handler_branch" ]; then git checkout -b ${pr_branch_name} origin/${pr_branch_name}; fi


cd /home/mono/workspace/eloqkv/eloq_metrics
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
set +e
eloq_metrics_branch=$(git branch -a | grep "remotes/origin/${pr_branch_name}" | grep -v grep)
set -e
if [ -n "$eloq_metrics_branch" ]; then git checkout -b ${pr_branch_name} origin/${pr_branch_name}; fi

cd /home/mono/workspace/
ln -s $WORKSPACE/eloq_test_src eloq_test
cd /home/mono/workspace/eloq_test/
set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
eloq_test_branch=$(git branch -a | grep "remotes/origin/${pr_branch_name}" | grep -v grep)
if [ -n "$eloq_test_branch" ]; then git checkout -b ${pr_branch_name} origin/${pr_branch_name}; fi
set -e

cd /home/mono/workspace/eloqkv

cmake_version=$(cmake --version 2>&1)
if [[ $? -eq 0 ]]; then
  echo "cmake version: $cmake_version"
else
  echo "fail to get cmake version"
fi


sudo apt-get update
sudo apt install apt-utils -y
sudo apt install python3-venv -y

# todo: move these code to docker-image
sudo apt install openssh-server -y
sudo service ssh start
cat /home/mono/.ssh/id_rsa.pub >> /home/mono/.ssh/authorized_keys
# disable ask when do ssh
sudo sed -i "s/#\s*StrictHostKeyChecking ask/    StrictHostKeyChecking no/g" /etc/ssh/ssh_config

python3 -m venv my_env
source my_env/bin/activate
pip install -r /home/mono/workspace/eloqkv/tests/unit/mono/log_replay_test/requirements.txt
deactivate

build_types=("Debug")
# kv_store_types=("CASSANDRA" "ROCKSDB")
kv_store_types=("ELOQDSS_ROCKSDB_CLOUD_S3" "ROCKSDB")

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
# run_eloq_ttl_tests TestsWithKV true true rocksdb
# # run_build "Debug" "CASSANDRA"
# # run_eloq_ttl_tests TestsWithLog true true cassandra
# deactivate
