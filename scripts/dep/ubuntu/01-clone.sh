#!/bin/bash
set -euo pipefail
set -x

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../../.." && pwd)
FETCH_SCRIPT="${REPO_ROOT}/data_substrate/scripts/third_party/fetch.sh"

if [ ! -x "${FETCH_SCRIPT}" ]; then
  echo "Missing ${FETCH_SCRIPT}. Initialize data_substrate first:" >&2
  echo "  git submodule update --init data_substrate" >&2
  exit 1
fi

"${FETCH_SCRIPT}" "$@"
