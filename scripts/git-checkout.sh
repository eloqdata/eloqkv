#!/usr/bin/bash
set -eo

if [ -n "$1" ]; then
  TAG=$1
else
  TAG="main"
fi

git checkout "${TAG}"
if [ "${TAG}" = "main" ]; then
  git pull origin main
fi
git submodule update --recursive
if [ "${TAG}" = "main" ]; then
  pushd log_service
  git checkout main
  git pull origin main
  git submodule update --recursive
  popd
  pushd tx_service/raft_host_manager
  git checkout main
  git pull origin main
  popd
else
  echo "-- checkout private submodules --"
  LOG_SERVICE_HASH=$(awk -F'=' '{ if ($1 == "log_service") {print $2} }' .private_modules)
  RAFT_HOST_MGR_HASH=$(awk -F'=' '{ if ($1 == "raft_host_manager") {print $2} }' .private_modules)
  pushd log_service
  git checkout ${LOG_SERVICE_HASH}
  git submodule update --recursive
  popd
  pushd tx_service/raft_host_manager
  git checkout ${RAFT_HOST_MGR_HASH}
  popd
fi
