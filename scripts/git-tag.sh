#!/usr/bin/bash
set -eo

TAG=$1
BRANCH_NAME="rel_${TAG//./_}_eloqkv"

# Utility: create and push a release branch for a module repo if available
create_and_push_release_branch() {
  local module_path="$1"
  local release_branch="$2"
  if git -C "$module_path" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    pushd "$module_path" >/dev/null
    git fetch origin '+refs/heads/*:refs/remotes/origin/*'
    if git ls-remote --heads origin "$release_branch" | grep -q "$release_branch"; then
      echo "Release branch $release_branch already exists for $module_path"
    else
      echo "Creating release branch $release_branch for $module_path"
      git checkout -b "$release_branch"
      git push -u origin "$release_branch"
    fi
    popd >/dev/null
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

# Check if the release branch exists, if not create it from main
if git ls-remote --heads origin "$BRANCH_NAME" | grep -q "$BRANCH_NAME"; then
  git checkout -b "$BRANCH_NAME" "origin/$BRANCH_NAME"
else
  git checkout -b "$BRANCH_NAME" main
fi

# Update version string in source
sed -i "s/constexpr char VERSION\[\] = \".*\";/constexpr char VERSION[] = \"${TAG}\";/" src/redis_server.cpp

# Commit version change if needed (no private metadata recorded in repo)
if [ -n "$(git diff --name-only src/redis_server.cpp)" ]; then
  git add src/redis_server.cpp
  git commit -m "New tag ${TAG}"
  git push origin "$BRANCH_NAME"
fi

create_and_push_release_branch "eloq_log_service" "$BRANCH_NAME" || true
create_and_push_release_branch "tx_service/raft_host_manager" "$BRANCH_NAME" || true

# Update the version on the tag (release) branch to ensure consistency
git checkout "$BRANCH_NAME"
sed -i "s/constexpr char VERSION\[\] = \".*\";/constexpr char VERSION[] = \"${TAG}\";/" src/redis_server.cpp
git add src/redis_server.cpp
git commit -m "Update version to ${TAG}" || true
git push origin "$BRANCH_NAME"
