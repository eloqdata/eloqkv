#!/usr/bin/env bash
# Compile every shipped eloqkv build variant against an already-built
# third-party prefix, timing each one. This is compile-only validation of the
# from-scratch build path — no functional tests are run here (ci.yml owns those).
#
# The third-party prefix is built once (all variants share it); each variant is
# then a fresh cmake configure + build in its own build dir. Build dirs are
# removed after each variant to keep runner disk bounded.
set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
cd "${REPO_ROOT}"

PREFIX="${ELOQ_THIRD_PARTY_PREFIX:-${REPO_ROOT}/data_substrate/third_party/install}"
JOBS="$(nproc)"

# id | WITH_DATA_STORE | WITH_LOG_STATE | extra cmake flags
# The 5 release-shipped variants (see .github/workflows/release.yml) plus
# rocks_gcs (ELOQDSS_ROCKSDB_CLOUD_GCS). RocksDB-backed data stores must be
# paired with the matching log state (data_substrate/CMakeLists.txt enforces it).
VARIANTS=(
  "rocksdb|ROCKSDB|ROCKSDB|"
  "rocks_s3|ELOQDSS_ROCKSDB_CLOUD_S3|ROCKSDB_CLOUD_S3|-DWITH_CLOUD_AZ_INFO=ON"
  "rocks_gcs|ELOQDSS_ROCKSDB_CLOUD_GCS|ROCKSDB_CLOUD_GCS|"
  "eloqstore_local|ELOQDSS_ELOQSTORE|ROCKSDB|"
  "eloqstore_s3|ELOQDSS_ELOQSTORE|ROCKSDB_CLOUD_S3|"
  "eloqstore_gcs|ELOQDSS_ELOQSTORE|ROCKSDB_CLOUD_GCS|"
)

summary_file="${GITHUB_STEP_SUMMARY:-/dev/stdout}"
{
  echo "## eloqkv compile matrix (plain ubuntu, third-party prefix: \`${PREFIX}\`)"
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
