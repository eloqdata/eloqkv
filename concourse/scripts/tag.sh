#!/bin/bash
set -exuo pipefail

export WORKSPACE=$PWD
sudo chown -R $USER $PWD
cd $HOME
ln -s ${WORKSPACE}/eloqkv_src eloqkv
cd eloqkv
ln -s $WORKSPACE/logservice_src log_service
pushd tx_service
ln -s $WORKSPACE/raft_host_manager_src raft_host_manager
popd

git config --global user.email "concourse@noreply.com"
git config --global user.name "concourse-ci"
# exit detach mode
git checkout -
git fetch --tags

latest=$(git tag --sort=-version:refname | head -n 1)

if [[ -z "$latest" ]]; then
    digits=(0 8 18)  # Initialize with 0.8.18 if no tags exist
else
    IFS='.' read -ra digits <<<"$latest"
fi

# Ensure we always have 3 parts
while [[ ${#digits[@]} -lt 3 ]]; do
    digits+=(0)
done

IFS='.' read -ra digits <<<"$latest"
case "$TAG_LEVEL" in
"major")
    ((++digits[0])) && digits[1]=0 && digits[2]=0
    ;;
"minor")
    ((++digits[1])) && digits[2]=0
    ;;
"patch") ((++digits[2])) ;;
*)
    echo "invalid tag level $TAG_LEVEL"
    exit 1
    ;;
esac
newtag=$(
    IFS='.'
    echo "${digits[*]}"
)

scripts/git-tag.sh $newtag