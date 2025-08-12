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
git submodule update --init --recursive

if [ "${TAG}" = "main" ]; then
  if [ -d eloq_log_service ]; then
    pushd eloq_log_service
    git checkout main
    git pull origin main
    git submodule update --init --recursive
    popd
  fi

  if [ -d tx_service/raft_host_manager ]; then
    pushd tx_service/raft_host_manager
    git checkout main
    git pull origin main
    popd
  fi
else
  REL_BRANCH="rel_${TAG//./_}_eloqkv"
  if [ -d eloq_log_service ]; then
    pushd eloq_log_service
    git fetch origin '+refs/heads/*:refs/remotes/origin/*'
    if git ls-remote --heads origin "$REL_BRANCH" | grep -q "$REL_BRANCH"; then
      git checkout -b "$REL_BRANCH" "origin/$REL_BRANCH"
    else
      echo "Expected release branch $REL_BRANCH not found in eloq_log_service"
      exit 1
    fi
    git submodule update --init --recursive
    popd
  fi

  if [ -d tx_service/raft_host_manager ]; then
    pushd tx_service/raft_host_manager
    git fetch origin '+refs/heads/*:refs/remotes/origin/*'
    if git ls-remote --heads origin "$REL_BRANCH" | grep -q "$REL_BRANCH"; then
      git checkout -b "$REL_BRANCH" "origin/$REL_BRANCH"
    else
      echo "Expected release branch $REL_BRANCH not found in tx_service/raft_host_manager"
      exit 1
    fi
    popd
  fi
fi
