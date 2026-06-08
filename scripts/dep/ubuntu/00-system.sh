#!/bin/bash
set -ex

# Ensure noninteractive apt; keep TZ default
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

# Install system packages
# Ensure curl is available for tool download
if ! command -v curl >/dev/null; then
    run_privileged apt-get update || true
    run_privileged apt-get install -y curl
fi

run_privileged apt-get update
run_privileged apt-get install -y --no-install-recommends \
    jq sudo vim wget curl apt-utils python3 python3-dev python3-pip python3-venv \
    python3-venv gdb libcurl4-openssl-dev build-essential libncurses5-dev \
    gnutls-dev bison zlib1g-dev ccache rsync cmake ninja-build libuv1-dev git \
    g++ make openjdk-11-jdk openssh-client openssh-server libssl-dev libgflags-dev \
    libleveldb-dev libsnappy-dev openssl lcov libbz2-dev liblz4-dev libzstd-dev \
    libboost-context-dev ca-certificates libc-ares-dev libc-ares2 m4 pkg-config \
    tar gcc redis tcl libreadline-dev ncurses-dev patchelf libprotobuf-dev \
    protobuf-compiler libjsoncpp-dev

# Install uv (Python package manager)
curl -LsSf https://astral.sh/uv/install.sh | sh
source $HOME/.cargo/env || true

# Install Google Cloud CLI
run_privileged apt-get install -y apt-transport-https ca-certificates gnupg
echo "deb https://packages.cloud.google.com/apt cloud-sdk main" | run_privileged tee -a /etc/apt/sources.list.d/google-cloud-sdk.list
curl https://packages.cloud.google.com/apt/doc/apt-key.gpg | run_privileged apt-key add -
run_privileged apt-get update && run_privileged apt-get install -y google-cloud-cli
