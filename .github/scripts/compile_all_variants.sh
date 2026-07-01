#!/usr/bin/env bash
# From-scratch compile validation for every shipped eloqkv data-store variant.
#
# Starting from a bare ubuntu:24.04 image this script:
#   1. installs the system + python dependencies (scripts/dep/ubuntu),
#   2. checks out the product + third-party submodules,
#   3. builds the third-party workspace from source into a shared prefix,
#   4. compiles every variant against that prefix (compile-only, no tests).
#
# It is compile-only validation of the from-scratch build path; ci.yml owns the
# functional test suite. Set ELOQ_SKIP_DEPS=1 / ELOQ_SKIP_THIRD_PARTY=1 to reuse
# an already-prepared environment (e.g. when iterating locally).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

JOBS="$(nproc)"
PREFIX="${ELOQ_THIRD_PARTY_PREFIX:-${REPO_ROOT}/data_substrate/third_party/install}"
export ELOQ_THIRD_PARTY_PREFIX="${PREFIX}"

# id | WITH_DATA_STORE | WITH_LOG_STATE | extra cmake flags
VARIANTS=(
  "rocksdb|ROCKSDB|ROCKSDB|"
  "rocks_s3|ELOQDSS_ROCKSDB_CLOUD_S3|ROCKSDB_CLOUD_S3|-DWITH_CLOUD_AZ_INFO=ON"
  "rocks_gcs|ELOQDSS_ROCKSDB_CLOUD_GCS|ROCKSDB_CLOUD_GCS|"
  "eloqstore_local|ELOQDSS_ELOQSTORE|ROCKSDB|"
  "eloqstore_s3|ELOQDSS_ELOQSTORE|ROCKSDB_CLOUD_S3|"
  "eloqstore_gcs|ELOQDSS_ELOQSTORE|ROCKSDB_CLOUD_GCS|"
)

if [ "${ELOQ_SKIP_DEPS:-0}" != "1" ]; then
  echo "::group::Install system + python dependencies"
  bash "${REPO_ROOT}/scripts/dep/ubuntu/00-system.sh"
  bash "${REPO_ROOT}/scripts/dep/ubuntu/02-python.sh" || true
  echo "::endgroup::"
fi

if [ "${ELOQ_SKIP_THIRD_PARTY:-0}" != "1" ]; then
  echo "::group::Build third-party workspace from source (prefix: ${PREFIX})"
  ELOQ_THIRD_PARTY_PREFIX="${PREFIX}" \
    bash "${REPO_ROOT}/data_substrate/scripts/third_party/install-ubuntu2404.sh"
  echo "::endgroup::"
fi

summary_file="${GITHUB_STEP_SUMMARY:-/dev/stdout}"
{
  echo "## eloqkv from-scratch compile matrix (ubuntu:24.04, prefix: \`${PREFIX}\`)"
  echo ""
  echo "| Variant | WITH_DATA_STORE | WITH_LOG_STATE | Result | Duration |"
  echo "|---|---|---|---|---|"
} >> "${summary_file}"

overall_status=0
for entry in "${VARIANTS[@]}"; do
  IFS='|' read -r id ds ls extra <<< "${entry}"
  build_dir="${REPO_ROOT}/build/${id}"

  echo "::group::Compile ${id} (WITH_DATA_STORE=${ds}, WITH_LOG_STATE=${ls})"
  start=$(date +%s)
  status="pass"
  # shellcheck disable=SC2086 -- ${extra} is an intentionally-splittable flag list
  if ! cmake -S "${REPO_ROOT}" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DWITH_DATA_STORE="${ds}" \
        -DWITH_LOG_STATE="${ls}" \
        -DWITH_LOG_SERVICE=ON \
        -DDISABLE_CKPT_REPORT=ON \
        -DDISABLE_CODE_LINE_IN_LOG=ON \
        -DWITH_ASAN=OFF \
        -DELOQ_THIRD_PARTY_PREFIX="${PREFIX}" \
        -DELOQ_THIRD_PARTY_REQUIRED=ON \
        ${extra} \
      || ! cmake --build "${build_dir}" -j "${JOBS}"; then
    status="FAIL"
    overall_status=1
  fi
  end=$(date +%s)
  dur=$(( end - start ))
  echo "::endgroup::"
  echo ">>> ${id}: ${status} in ${dur}s"
  printf '| %s | %s | %s | %s | %ds |\n' \
    "${id}" "${ds}" "${ls}" "${status}" "${dur}" >> "${summary_file}"

  # Compile-only: drop the build tree to keep runner disk in check.
  rm -rf "${build_dir}"
done

if [ "${overall_status}" -ne 0 ]; then
  echo "One or more variants failed to compile." >&2
fi
exit "${overall_status}"
