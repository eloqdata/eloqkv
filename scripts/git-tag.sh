#!/usr/bin/bash
set -eo
TAG=$1
BRANCH_NAME="rel_${TAG//./_}"

set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
set -e
git checkout -b $BRANCH_NAME origin/$BRANCH_NAME
sed -i "s/constexpr char VERSION\[\] = ".*";/constexpr char VERSION[] = \"${TAG}\";/" src/redis_server.cpp
pushd log_service
set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
set -e
git checkout -b $BRANCH_NAME origin/$BRANCH_NAME
LOG_SERVICE_HASH=$(git rev-parse HEAD)
popd
pushd tx_service/raft_host_manager
set +e
git fetch origin '+refs/heads/*:refs/remotes/origin/*'
set -e
git checkout -b $BRANCH_NAME origin/$BRANCH_NAME
RAFT_HOST_MGR_HASH=$(git rev-parse HEAD)
popd
echo "log_service=${LOG_SERVICE_HASH}" >.private_modules
echo "raft_host_manager=${RAFT_HOST_MGR_HASH}" >>.private_modules
if [ -n "$(git diff --name-only src/redis_server.cpp .private_modules)" ]; then
    git add src/redis_server.cpp .private_modules
    git commit -m "New tag $TAG"
    git push origin $BRANCH_NAME
fi
git tag $TAG
git push origin $TAG

# Update the version on main branch
git checkout main
sed -i "s/constexpr char VERSION\[\] = ".*";/constexpr char VERSION[] = \"${TAG}\";/" src/redis_server.cpp
git add src/redis_server.cpp
git commit -m "Update version to $TAG"
git push
