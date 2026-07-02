#!/bin/bash
set -euo pipefail
set -x

# Installs the build dependencies for a from-scratch (bare ubuntu:24.04) build:
# system packages + the third-party workspace. Python test/runtime tooling
# (awscli, boto3, grpcio-tools, ...) is NOT a build dependency and lives in the
# eloqdata/ubuntu-dev image instead, so it is intentionally not installed here.

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
THIRD_PARTY_INSTALLER="${REPO_ROOT}/data_substrate/scripts/third_party/install-ubuntu2404.sh"

if [ ! -x "${THIRD_PARTY_INSTALLER}" ]; then
  echo "Missing ${THIRD_PARTY_INSTALLER}. Initialize data_substrate first:" >&2
  echo "  git submodule update --init data_substrate" >&2
  exit 1
fi

# --- System packages ---
export DEBIAN_FRONTEND=noninteractive
export TZ=${TZ:-UTC}

run_privileged() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  else
    echo "This script must run as root or with sudo available: $*" >&2
    exit 1
  fi
}

needs_tz_config=false
if [ ! -f /etc/timezone ] || ! grep -qE '^(Etc/UTC|UTC)$' /etc/timezone; then
  needs_tz_config=true
fi
if [ ! -L /etc/localtime ] || [ "$(readlink -f /etc/localtime)" != "/usr/share/zoneinfo/Etc/UTC" ]; then
  needs_tz_config=true
fi

if $needs_tz_config; then
  echo 'tzdata tzdata/Areas select Etc' | run_privileged debconf-set-selections || true
  echo 'tzdata tzdata/Zones/Etc select UTC' | run_privileged debconf-set-selections || true
  echo 'Etc/UTC' | run_privileged tee /etc/timezone >/dev/null
  run_privileged ln -sf /usr/share/zoneinfo/Etc/UTC /etc/localtime
fi

# Ensure curl is available for tool download
if ! command -v curl >/dev/null; then
    run_privileged apt-get update || true
    run_privileged apt-get install -y curl
fi

run_privileged apt-get update
# Build dependencies only. Test/dev tooling (gdb, lcov, redis, tcl, openjdk,
# openssh-server, ...) is not needed to compile and lives in the ubuntu-dev
# image, so it is intentionally excluded here.
run_privileged apt-get install -y --no-install-recommends \
    sudo wget curl apt-utils python3 python3-dev python3-pip python3-venv \
    libcurl4-openssl-dev build-essential libncurses5-dev \
    gnutls-dev bison zlib1g-dev ccache rsync cmake ninja-build libuv1-dev git \
    g++ make openssh-client libssl-dev libgflags-dev \
    libleveldb-dev libsnappy-dev openssl libbz2-dev liblz4-dev libzstd-dev \
    libboost-context-dev ca-certificates libc-ares-dev libc-ares2 m4 pkg-config \
    tar gcc libreadline-dev ncurses-dev patchelf libprotobuf-dev \
    protobuf-compiler libjsoncpp-dev

# --- Third-party workspace ---
"${THIRD_PARTY_INSTALLER}"
