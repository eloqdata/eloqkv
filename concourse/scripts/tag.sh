#!/bin/bash
set -exuo pipefail

export WORKSPACE=$PWD
sudo chown -R $USER $PWD
cd $HOME
ln -s ${WORKSPACE}/eloqkv_src eloqkv
cd eloqkv
# Prefer linking to expected path used by build scripts: eloq_log_service
if [ -d "$WORKSPACE/logservice_src" ]; then
  ln -s $WORKSPACE/logservice_src data_substrate/eloq_log_service
fi
pushd data_substrate/tx_service
ln -s $WORKSPACE/raft_host_manager_src raft_host_manager
popd

pushd data_substrate/store_handler/eloq_data_store_service
ln -s $WORKSPACE/eloq_store_src eloqstore
popd

git config --global user.email "concourse@noreply.com"
git config --global user.name "concourse-ci"

# Configure git to use SSH key for authentication
mkdir -p ~/.ssh
echo "$GIT_SSH_KEY" > ~/.ssh/id_rsa
chmod 600 ~/.ssh/id_rsa
ssh-keyscan github.com >> ~/.ssh/known_hosts

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