#!/bin/bash
set -exo pipefail

# Open-log build for EloqKV on Ubuntu 24.04 using install_dependency_ubuntu2404.sh

export WORKSPACE=$PWD

# Minimal tools required before dependency installer (plain ubuntu image)
apt-get update && apt-get install -y sudo openssh-client ca-certificates rsync

current_user=$(whoami)
sudo chown -R "$current_user" "$PWD"

# SSH for private repos
if [ -n "${GIT_SSH_KEY}" ]; then
  mkdir -p ~/.ssh
  echo "${GIT_SSH_KEY}" > ~/.ssh/id_rsa
  chmod 600 ~/.ssh/id_rsa
  ssh-keyscan github.com >> ~/.ssh/known_hosts 2>/dev/null
fi

# Defaults
: "${BUILD_TYPE:=Debug}"
: "${ASAN:=OFF}"
: "${DATA_STORE_TYPE:=ROCKSDB}"
: "${NCORE:=8}"
: "${OUT_NAME:=debug-openlog}"

# Workspace layout
cd "$HOME"
ln -sfn "${WORKSPACE}/eloqkv_src" eloqkv
ELOQKV_SRC="${HOME}/eloqkv"

# ensure log service under expected path
if [ -d "${WORKSPACE}/logservice_src" ]; then
  ln -sfn "${WORKSPACE}/logservice_src" "${ELOQKV_SRC}/data_substrate/log_service"
fi

pushd "${ELOQKV_SRC}/data_substrate/tx_service" >/dev/null
ln -sfn "${WORKSPACE}/raft_host_manager_src" raft_host_manager
popd >/dev/null

# Install dependencies for Ubuntu 24.04
pushd "${ELOQKV_SRC}" >/dev/null
bash scripts/install_dependency_ubuntu2404.sh
source "$HOME/venv/bin/activate"
popd >/dev/null

# Helper to copy dependent libraries for portability
copy_libraries() {
  local executable="$1"
  local path="$2"
  libraries=$(ldd "$executable" | awk 'NF==4{print $(NF-1)}{}')
  mkdir -p "$path"
  for lib in $libraries; do
    rsync -avL --ignore-existing "$lib" "$path/"
  done
}

# Create destination layout
DEST_DIR="${HOME}/EloqKV"
rm -rf "${DEST_DIR}"
mkdir -p "${DEST_DIR}/bin" "${DEST_DIR}/lib" "${DEST_DIR}/conf"

# Build eloqkv with open log service
cd "$ELOQKV_SRC"
rm -rf build
mkdir build && cd build

cmake .. \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DWITH_DATA_STORE="${DATA_STORE_TYPE}" \
  -DWITH_LOG_SERVICE=ON \
  -DOPEN_LOG_SERVICE=ON \
  -DDISABLE_CKPT_REPORT=ON \
  -DDISABLE_CODE_LINE_IN_LOG=ON \
  -DFORK_HM_PROCESS=OFF \
  -DWITH_ASAN="${ASAN}"

cmake --build . --config "${BUILD_TYPE}" -j"${NCORE}"

# Package executables and runtime libs
copy_libraries eloqkv "${DEST_DIR}/lib"
mv eloqkv "${DEST_DIR}/bin/"
copy_libraries host_manager "${DEST_DIR}/lib"
mv host_manager "${DEST_DIR}/bin/"

# If ROCKSDB tools are built
if [ "${DATA_STORE_TYPE}" = "ROCKSDB" ]; then
  if [ -f eloqkv_to_aof ]; then
    copy_libraries eloqkv_to_aof "${DEST_DIR}/lib"
    mv eloqkv_to_aof "${DEST_DIR}/bin/"
  fi
  if [ -f eloqkv_to_rdb ]; then
    copy_libraries eloqkv_to_rdb "${DEST_DIR}/lib"
    mv eloqkv_to_rdb "${DEST_DIR}/bin/"
  fi
fi

# Fix rpath for common prebuilt shared libs if present
if command -v patchelf >/dev/null 2>&1; then
  for libpat in libleveldb.* libbrpc.* libbraft.* librocksdb*; do
    if compgen -G "${DEST_DIR}/lib/${libpat}" > /dev/null; then
      patchelf --set-rpath '$ORIGIN' ${DEST_DIR}/lib/${libpat}
    fi
  done
fi

# Include default config if present
if [ -f "${ELOQKV_SRC}/concourse/artifact/eloqkv.ini" ]; then
  cp "${ELOQKV_SRC}/concourse/artifact/eloqkv.ini" "${DEST_DIR}/conf/"
fi

# Build and include open log_service
LOG_SV_SRC="${ELOQKV_SRC}/data_substrate/log_service"
if [ -d "${LOG_SV_SRC}" ]; then
  pushd "${LOG_SV_SRC}" >/dev/null
  rm -rf build LogService
  mkdir -p LogService/bin LogService/lib
  mkdir build && cd build
  cmake .. -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DWITH_ASAN="${ASAN}" -DDISABLE_CODE_LINE_IN_LOG=ON
  cmake --build . --config "${BUILD_TYPE}" -j"${NCORE}"
  mv "${LOG_SV_SRC}/build/launch_sv" "${LOG_SV_SRC}/LogService/bin"
  if [ -f "${LOG_SV_SRC}/LogService/bin/launch_sv" ]; then
    copy_libraries "${LOG_SV_SRC}/LogService/bin/launch_sv" "${LOG_SV_SRC}/LogService/lib"
    # also place into main package for convenience
    copy_libraries "${LOG_SV_SRC}/LogService/bin/launch_sv" "${DEST_DIR}/lib"
    cp "${LOG_SV_SRC}/LogService/bin/launch_sv" "${DEST_DIR}/bin/"
  fi
  popd >/dev/null
fi

# Tarball name
OS_ID="ubuntu24"
ARCH_ID=$(uname -m)
case "$ARCH_ID" in
  amd64|x86_64) ARCH_ID=amd64 ;;
  arm64|aarch64) ARCH_ID=arm64 ;;
esac
TARBALL_NAME="eloqkv-${OUT_NAME}-${OS_ID}-${ARCH_ID}.tar.gz"

# Create tarball
cd "${HOME}"
tar -czvf "$TARBALL_NAME" -C "${HOME}" EloqKV

# Expose artifact to Concourse output
mkdir -p "${WORKSPACE}/the-output"
mv "$TARBALL_NAME" "${WORKSPACE}/the-output/"

# Cleanup build directories (leave the-output intact)
cd "$ELOQKV_SRC"
rm -rf build
if [ -d "${LOG_SV_SRC}" ]; then
  rm -rf "${LOG_SV_SRC}/build" "${LOG_SV_SRC}/LogService"
fi

echo "Build completed. Tarball at the-output/${TARBALL_NAME}"
