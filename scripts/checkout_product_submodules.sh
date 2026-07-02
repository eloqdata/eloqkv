#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

cd "${REPO_ROOT}"

git submodule update --init crcspeed data_substrate

git -C data_substrate submodule update --init \
  tx_service/tx-log-protos \
  log_service \
  store_handler/eloq_data_store_service/eloqstore

ELOQSTORE_DIR=data_substrate/store_handler/eloq_data_store_service/eloqstore
if [ -f "${ELOQSTORE_DIR}/.gitmodules" ]; then
  git -C "${ELOQSTORE_DIR}" submodule update --init \
    external/concurrentqueue \
    external/inih \
    external/abseil
fi
