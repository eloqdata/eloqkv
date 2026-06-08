#!/bin/bash
set -euo pipefail
set -x

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
THIRD_PARTY_INSTALLER="${REPO_ROOT}/data_substrate/scripts/third_party/install-ubuntu2404.sh"

if [ ! -x "${THIRD_PARTY_INSTALLER}" ]; then
  echo "Missing ${THIRD_PARTY_INSTALLER}. Initialize data_substrate first:" >&2
  echo "  git submodule update --init data_substrate" >&2
  exit 1
fi

bash "${SCRIPT_DIR}/dep/ubuntu/00-system.sh"

bash "${SCRIPT_DIR}/dep/ubuntu/02-python.sh" &
python_pid=$!
"${THIRD_PARTY_INSTALLER}" &
third_party_pid=$!

python_status=0
third_party_status=0
wait "${python_pid}" || python_status=$?
wait "${third_party_pid}" || third_party_status=$?

if [ "${python_status}" -ne 0 ] || [ "${third_party_status}" -ne 0 ]; then
  exit 1
fi

if [ -f "${HOME}/venv/bin/activate" ]; then
  # shellcheck disable=SC1091
  source "${HOME}/venv/bin/activate"
fi
