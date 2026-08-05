#!/usr/bin/env bash
#
# Measure line coverage of the paged-object feature code and enforce the
# 100% criterion (docs/08-paged-objects-plan.md "Testing").
#
#   tests/unit_cc/paged_coverage.sh [build_dir]     # default: bld
#
# Scope: the six files that ARE the feature — the engine page-management
# layer and the hash type. Code the feature threads through pre-existing
# engine files (the drain, the fetch hub, the swap rule, deferred promotion)
# is exercised by the server-level scenario tests instead; line-measuring it
# would mean instrumenting a full server build.
#
# Exclusion policy (each site carries an inline GCOVR_EXCL marker with its
# reason; nothing else may be excluded):
#   - failure arms of invariant SELF-CHECKS (CheckLruInvariants,
#     CheckCanonical, CheckInvariants): no public mutator can violate the
#     invariant, which passing tests prove; the arms exist to catch future
#     regressions.
#   - closing braces carrying destructor code that guaranteed copy elision
#     makes unreachable (a gcc artifact on NRVO returns).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BLD="${1:-$REPO/bld}"
export LD_LIBRARY_PATH="$REPO/data_substrate/third_party/install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cd "$BLD"
cmake "$REPO" -DPAGED_TEST_COVERAGE=ON > /dev/null
cmake --build . --parallel 8 --target \
    paged_hash_core_test paged_flush_roundtrip_test \
    paged_eviction_test paged_object_command_test > /dev/null

find . -name '*.gcda' -delete
for t in paged_hash_core_test paged_eviction_test \
         paged_flush_roundtrip_test paged_object_command_test; do
    ./"$t" > /dev/null
done

gcovr -r "$REPO" --gcov-executable gcov-15 --object-directory . \
    --filter '.*/(page_frame_table|paged_tx_object|redis_paged_hash_core|redis_paged_hash_object|redis_paged_defs|page_key_codec)\.h' \
    --txt --fail-under-line 100

# Leave the tree configured without instrumentation.
cmake "$REPO" -DPAGED_TEST_COVERAGE=OFF > /dev/null
echo "paged coverage: 100% (fail-under would have errored otherwise)"
