#!/usr/bin/bash
set -eo

TAG=$1
REL_BRANCH="rel_${TAG//./_}_eloqkv"

# Utility: create and push a release branch for a module repo if available
create_and_push_release_branch() {
  local module_path="$1"
  local release_branch="$2"
  if [ -d "$module_path" ]; then
    pushd "$module_path"
    git fetch origin '+refs/heads/*:refs/remotes/origin/*'
    if git show-ref --verify --quiet "refs/heads/$release_branch" || \
       git ls-remote --heads origin "$release_branch" | grep -q "$release_branch"; then
      echo "Error: release branch $release_branch already exists for $module_path (local or remote)" >&2
      popd
      exit 1
    fi
    echo "Creating release branch $release_branch for $module_path"
    git checkout -b "$release_branch"
    git push -u origin "$release_branch"
    popd
  else
    echo "Error: module path $module_path does not exist" >&2
    exit 1
  fi
}

set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
set -e

# Ensure we're on main branch first (checkout remote main if local doesn't exist)
if git show-ref --verify --quiet refs/heads/main; then
  git checkout main
else
  git checkout -b main origin/main
fi

# Validate release branch does not already exist (local or remote), then create from main
if git show-ref --verify --quiet "refs/heads/$REL_BRANCH" || \
   git ls-remote --heads origin "$REL_BRANCH" | grep -q "$REL_BRANCH"; then
  echo "Error: release branch $REL_BRANCH already exists (local or remote)" >&2
  exit 1
fi
git checkout -b "$REL_BRANCH" main

# Update version string in source
sed -i "s/constexpr char VERSION\[\] = \".*\";/constexpr char VERSION[] = \"${TAG}\";/" src/redis_server.cpp

if [ -n "$(git diff --name-only src/redis_server.cpp)" ]; then
  git add src/redis_server.cpp
  git commit -m "New tag ${TAG}"
  git push origin "$REL_BRANCH"
fi

create_and_push_release_branch "data_substrate/eloq_log_service" "$REL_BRANCH"
create_and_push_release_branch "data_substrate/tx_service/raft_host_manager" "$REL_BRANCH"
