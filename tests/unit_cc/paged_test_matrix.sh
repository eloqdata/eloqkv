#!/usr/bin/env bash
# Run the paged-object suite in EloqStore deployment lanes S0/S1/P0/P1.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LANE=all
TIER=pr
SEED=57612
REPEAT=1
KEEP=0
FAILED=0
INCONCLUSIVE=0
RESULTS="$REPO/bld-paged-matrix/results.tsv"
THIRD_PARTY_LIB="$REPO/data_substrate/third_party/install/lib"
REFERENCE_ROOT="$REPO/bld-paged-matrix/reference"
REFERENCE_PORT=7301
REFERENCE_ACTIVE=0
export LD_LIBRARY_PATH="$THIRD_PARTY_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --lane) LANE="$2"; shift 2 ;;
        --tier) TIER="$2"; shift 2 ;;
        --seed) SEED="$2"; shift 2 ;;
        --repeat) REPEAT="$2"; shift 2 ;;
        --keep-artifacts-on-failure) KEEP=1; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
case "$LANE" in S0|S1|P0|P1|all) ;; *) echo "invalid --lane $LANE" >&2; exit 2 ;; esac
case "$TIER" in smoke|pr|nightly|release) ;; *) echo "invalid --tier $TIER" >&2; exit 2 ;; esac
if [ "$TIER" = release ] && [ "$REPEAT" -lt 2 ]; then
    REPEAT=2
fi

mkdir -p "$(dirname "$RESULTS")"
: > "$RESULTS"
ACTIVE_HARNESS=""

cleanup() {
    if [ "$FAILED" -ne 0 ] && [ "$KEEP" -eq 1 ]; then
        echo "failure artifacts retained (--keep-artifacts-on-failure)"
        return
    fi
    case "$ACTIVE_HARNESS" in
        single) "$REPO/tests/unit_cc/paged_single_node.sh" down || true ;;
        cluster) "$REPO/tests/unit_cc/standby_cluster.sh" down || true ;;
        production)
            ELOQKV_PAGED_SINGLE_ROOT="$REPO/bld-paged-matrix/production-config" \
            ELOQKV_PAGED_SINGLE_PORT=7398 \
                "$REPO/tests/unit_cc/paged_single_node.sh" down || true
            ;;
    esac
    if [ "$REFERENCE_ACTIVE" -eq 1 ]; then
        ELOQKV_PAGED_SINGLE_ROOT="$REFERENCE_ROOT" \
        ELOQKV_PAGED_SINGLE_PORT="$REFERENCE_PORT" \
            "$REPO/tests/unit_cc/paged_single_node.sh" down || true
    fi
}
trap cleanup EXIT INT TERM

record() {
    local lane="$1" scenario="$2" status="$3"
    printf '%s\t%s\t%s\tseed=%s\ttier=%s\n' \
        "$lane" "$scenario" "$status" "$SEED" "$TIER" | tee -a "$RESULTS"
}

archive_failure() {
    local lane="$1" scenario="$2" stamp archive
    local RUN_DIR="" START_SCRIPT="" START_DARK_SCRIPT="" LOG_DIR=""
    local PORT="" WAL="" PAGED_THRESHOLD="" PAGED_PAGE_SIZE=""
    local NODE_MEMORY_MB=""
    stamp="$(date +%Y%m%d-%H%M%S)"
    archive="$REPO/bld-paged-matrix/failure-artifacts/${lane}-${scenario}-${SEED}-${stamp}"
    mkdir -p "$archive"
    case "$ACTIVE_HARNESS" in
        single)
            if [ -f "$REPO/bld-paged-matrix/single/current.env" ]; then
                # shellcheck disable=SC1091
                source "$REPO/bld-paged-matrix/single/current.env"
                cp -a "$RUN_DIR/eloqkv.ini" "$RUN_DIR/start.sh" \
                    "$RUN_DIR/start-dark.sh" "$RUN_DIR/eloqkv.stdout" \
                    "$RUN_DIR/bootstrap.stdout" "$RUN_DIR/logs" \
                    "$RUN_DIR/bootstrap-logs" "$archive/" 2>/dev/null || true
            fi
            ;;
        cluster)
            cp -a "$REPO/bld-standby/primary.ini" \
                "$REPO/bld-standby/standby.ini" \
                "$REPO/bld-standby/start_p.sh" \
                "$REPO/bld-standby/start_s.sh" \
                "$REPO/bld-standby/p_stdout.log" \
                "$REPO/bld-standby/s_stdout.log" \
                "$REPO/bld-standby/bootstrap.log" \
                "$REPO/bld-standby/logsv_stdout.log" \
                "$REPO/bld-standby/p_log" "$REPO/bld-standby/s_log" \
                "$REPO/bld-standby/logsv/glog" "$archive/" \
                2>/dev/null || true
            ;;
        production)
            if [ -f "$REPO/bld-paged-matrix/production-config/current.env" ]; then
                # shellcheck disable=SC1091
                source "$REPO/bld-paged-matrix/production-config/current.env"
                cp -a "$RUN_DIR/eloqkv.ini" "$RUN_DIR/start.sh" \
                    "$RUN_DIR/eloqkv.stdout" "$RUN_DIR/bootstrap.stdout" \
                    "$RUN_DIR/logs" "$RUN_DIR/bootstrap-logs" "$archive/" \
                    2>/dev/null || true
            fi
            ;;
    esac
    printf 'failure artifacts: %s\n' "$archive"
}

run_case() {
    local lane="$1" scenario="$2"; shift 2
    local rc=0
    "$@" || rc=$?
    if [ "$rc" -eq 0 ]; then
        record "$lane" "$scenario" pass
    else
        FAILED=1
        record "$lane" "$scenario" fail
        archive_failure "$lane" "$scenario"
        # Keep qualifying independent scenarios. The matrix returns failure
        # after cleanup, but one semantic mismatch must not mask later crash,
        # recovery, or deployment-specific coverage.
        return 0
    fi
}

run_case_inconclusive() {
    local lane="$1" scenario="$2"; shift 2
    local rc=0
    "$@" || rc=$?
    case "$rc" in
        0) record "$lane" "$scenario" pass ;;
        2)
            INCONCLUSIVE=1
            record "$lane" "$scenario" inconclusive
            archive_failure "$lane" "$scenario"
            [ "$TIER" != release ] || FAILED=1
            ;;
        *)
            FAILED=1
            record "$lane" "$scenario" fail
            archive_failure "$lane" "$scenario"
            ;;
    esac
}

tier_at_least() {
    local want="$1" actual=0 minimum=0
    case "$TIER" in smoke) actual=0 ;; pr) actual=1 ;; nightly) actual=2 ;; release) actual=3 ;; esac
    case "$want" in smoke) minimum=0 ;; pr) minimum=1 ;; nightly) minimum=2 ;; release) minimum=3 ;; esac
    [ "$actual" -ge "$minimum" ]
}

start_reference() {
    ELOQKV_PAGED_SINGLE_ROOT="$REFERENCE_ROOT" \
    ELOQKV_PAGED_SINGLE_PORT="$REFERENCE_PORT" \
    ELOQKV_PAGED_LOGSV_PORT=19081 \
    ELOQKV_PAGED_THRESHOLD=0 \
        "$REPO/tests/unit_cc/paged_single_node.sh" up --wal off \
        --tag "reference-$SEED-$(date +%H%M%S)"
    REFERENCE_ACTIVE=1
}

stop_reference() {
    ELOQKV_PAGED_SINGLE_ROOT="$REFERENCE_ROOT" \
    ELOQKV_PAGED_SINGLE_PORT="$REFERENCE_PORT" \
        "$REPO/tests/unit_cc/paged_single_node.sh" down
    REFERENCE_ACTIVE=0
}

run_units() {
    cmake --build "$REPO/bld" --parallel 4 --target \
        paged_hash_core_test paged_flush_roundtrip_test \
        paged_eviction_test paged_object_command_test paged_codec_fuzz_test \
        paged_backfill_engine_test \
        PageFetch-Test LockGrantability-Test PagedStoreBatch-Test
    local test
    run_case unit paged-testlib python3 \
        "$REPO/tests/unit_cc/paged_testlib_test.py"
    run_case unit eloqstore-paged-rows python3 \
        "$REPO/tests/unit_cc/eloqstore_paged_rows_test.py"
    for test in paged_hash_core_test paged_flush_roundtrip_test \
                paged_eviction_test paged_object_command_test; do
        run_case unit "$test" "$REPO/bld/$test"
    done
    local fuzz_iterations=10000
    tier_at_least nightly && fuzz_iterations=100000
    tier_at_least release && fuzz_iterations=500000
    run_case unit paged-codec-corpus "$REPO/bld/paged_codec_fuzz_test" \
        --seed "$SEED" --iterations "$fuzz_iterations"
    run_case unit paged-backfill-engine \
        "$REPO/bld/paged_backfill_engine_test"
    run_case unit PageFetch-Test "$REPO/bld/data_substrate/tx_service/tests/PageFetch-Test"
    run_case unit LockGrantability-Test \
        "$REPO/bld/data_substrate/tx_service/tests/LockGrantability-Test"
    run_case unit PagedStoreBatch-Test \
        "$REPO/bld/data_substrate/tx_service/tests/PagedStoreBatch-Test"
}

run_upstream_hash() {
    local lane="$1" port="$2"
    run_case "$lane" upstream-hash-tcl tclsh "$REPO/tests/test_helper.tcl" \
        --host 127.0.0.1 --port "$port" \
        --tags -needs:repl --tags -needs:config-maxmemory \
        --tags -needs:debug --tags -needs:redis_config \
        --tags -needs:redis_expire --tags -needs:slow_test \
        --tags -needs:support_cmd_later --tags -needs:cluster_mode \
        --single /unit/eloq/hash
}

run_common() {
    local lane="$1" port="$2" log_dir="$3"
    local hash_extra=()
    case "$lane" in P0|P1) hash_extra+=(--skip-ttl) ;; esac
    run_case "$lane" hash-model python3 "$REPO/tests/unit_cc/paged_hash_matrix.py" \
        --port "$port" --seed "$SEED" --faults --log-dir "$log_dir" \
        "${hash_extra[@]}"
    run_case "$lane" conversion python3 "$REPO/tests/unit_cc/paged_conversion_policy.py" "$port"
    run_case "$lane" hscan python3 "$REPO/tests/unit_cc/paged_hscan.py" "$port" 800
    run_case "$lane" hlen python3 "$REPO/tests/unit_cc/paged_hlen_consistency.py" 800 "$port"
    run_case "$lane" scan-keys-isolation python3 "$REPO/tests/unit_cc/paged_scan_leak.py" "$port" 8
    if tier_at_least pr; then
        run_case "$lane" key-security python3 \
            "$REPO/tests/unit_cc/paged_key_security.py" "$port"
        run_case "$lane" oversized-record python3 \
            "$REPO/tests/unit_cc/paged_oversized_record.py" "$port" 4096
        run_case "$lane" concurrency python3 "$REPO/tests/unit_cc/paged_concurrency_matrix.py" \
            --port "$port" --log-dir "$log_dir" --checkpoint-wait 8
        run_case "$lane" corrupt-page python3 "$REPO/tests/unit_cc/paged_corrupt_page.py" \
            "$port" "$log_dir"
        run_case "$lane" page-fault-matrix python3 \
            "$REPO/tests/unit_cc/paged_fault_matrix.py" "$log_dir" "$port"
        run_case "$lane" store-failure-matrix python3 \
            "$REPO/tests/unit_cc/paged_store_failure_matrix.py" \
            --port "$port" --log-dir "$log_dir" --checkpoint-wait 8
        case "$lane" in
            S1|P1)
                run_case "$lane" commit-memory-park python3 \
                    "$REPO/tests/unit_cc/paged_commit_memory_park.py" \
                    "$port" "$log_dir" --wal-enabled
                ;;
        esac
    fi
    if tier_at_least nightly; then
        run_upstream_hash "$lane" "$port"
        start_reference
        local differential_steps=1000
        [ "$TIER" = release ] && differential_steps=10000
        run_case "$lane" protocol-differential python3 \
            "$REPO/tests/unit_cc/paged_protocol_differential.py" \
            --paged-port "$port" --reference-port "$REFERENCE_PORT" \
            --log-dir "$log_dir" --seed "$SEED" \
            --steps "$differential_steps" --checkpoint-wait 8 \
            --trace-out "$REPO/bld-paged-matrix/${lane,,}-differential-$SEED.json"
        stop_reference
    fi
}

run_single_lane() {
    local lane="$1" wal=off
    [ "$lane" = S1 ] && wal=on
    ACTIVE_HARNESS=single
    "$REPO/tests/unit_cc/paged_single_node.sh" up --wal "$wal" \
        --tag "${lane,,}-$SEED-$(date +%H%M%S)"
    # shellcheck disable=SC1091
    source "$REPO/bld-paged-matrix/single/current.env"
    run_common "$lane" "$PORT" "$LOG_DIR"
    if tier_at_least pr; then
        run_case "$lane" lifecycle python3 \
            "$REPO/tests/unit_cc/paged_lifecycle_matrix.py" \
            --mode single --port "$PORT" --log-dir "$LOG_DIR" \
            --start-script "$START_SCRIPT" --checkpoint-wait 15
        run_case "$lane" ttl-matrix python3 \
            "$REPO/tests/unit_cc/paged_ttl_matrix.py" --port "$PORT" \
            --log-dir "$LOG_DIR" --start-script "$START_SCRIPT" \
            --checkpoint-wait 15
    fi
    if tier_at_least nightly; then
        run_case "$lane" persistence-matrix python3 \
            "$REPO/tests/unit_cc/paged_persistence_matrix.py" \
            --port "$PORT" --start-script "$START_SCRIPT" \
            --run-dir "$RUN_DIR" --log-dir "$LOG_DIR" --wal "$wal" \
            --checkpoint-wait 12
        local dss_port=$((PORT + 10007))
        run_case "$lane" durable-store-rows python3 \
            "$REPO/tests/unit_cc/paged_store_rows_oracle.py" \
            --port "$PORT" --dss-port "$dss_port" --checkpoint-wait 12
        run_case "$lane" durable-corruption python3 \
            "$REPO/tests/unit_cc/paged_durable_corruption.py" \
            --port "$PORT" --dss-port "$dss_port" \
            --start-script "$START_SCRIPT" --run-dir "$RUN_DIR" \
            --log-dir "$LOG_DIR" --checkpoint-wait 12
        run_case "$lane" flush-crash-points python3 \
            "$REPO/tests/unit_cc/paged_flush_crash_points.py" \
            --port "$PORT" --start-script "$START_SCRIPT" \
            --run-dir "$RUN_DIR" --log-dir "$LOG_DIR" --wal "$wal" \
            --checkpoint-wait 12
    fi
    if [ "$lane" = S1 ] && tier_at_least pr; then
        run_case "$lane" crash-replay python3 \
            "$REPO/tests/unit_cc/replay_paged_restart.py" "$START_SCRIPT" "$LOG_DIR"
        run_case "$lane" corrupt-record python3 \
            "$REPO/tests/unit_cc/paged_corrupt_record.py" "$START_SCRIPT" "$PORT"
        # Acknowledged DEL -> SIGKILL before its checkpoint -> replay. Asserts
        # the unknown-prior deletion CONTRACT (key gone, metadata row gone,
        # recreatable; leftover page rows are accepted sweeper debt). Kills
        # and restarts the node via START_SCRIPT, so it sits with the other
        # restart cases.
        run_case "$lane" replay-delete-rows python3 \
            "$REPO/tests/unit_cc/paged_replay_delete_rows.py" \
            --port "$PORT" --dss-port "$((PORT + 10007))" \
            --start-script "$START_SCRIPT" --checkpoint-wait 12
    fi
    if [ "$lane" = S1 ] && tier_at_least nightly; then
        run_case_inconclusive "$lane" replay-serving-window python3 \
            "$REPO/tests/unit_cc/replay_serving_window.py" \
            "$START_SCRIPT" "$LOG_DIR"
    fi
    if tier_at_least nightly; then
        run_case "$lane" swap-inflight-fetch python3 \
            "$REPO/tests/unit_cc/swap_with_inflight_fetch.py"
        run_case "$lane" abort-parked-fetch python3 \
            "$REPO/tests/unit_cc/abort_with_parked_command.py"
        local soak_steps=5000
        [ "$TIER" = release ] && soak_steps=50000
        run_case "$lane" soak python3 "$REPO/tests/unit_cc/paged_soak.py" \
            --port "$PORT" --log-dir "$LOG_DIR" --seed "$SEED" \
            --steps "$soak_steps" --checkpoint-wait 8
    fi
    # Run last because a regression in the admission-refusal state machine can
    # abort a Debug server; cleanup should follow immediately instead of
    # turning every later scenario into a secondary connection failure.
    if tier_at_least pr; then
        run_case "$lane" memory-pressure python3 \
            "$REPO/tests/unit_cc/paged_memory_pressure_matrix.py" \
            --port "$PORT" --log-dir "$LOG_DIR" --checkpoint-wait 8
        # >20 memory waiters through a campaign's TERMINAL branch. Runs after
        # the pressure matrix for the same isolation reason: it arms
        # admission refusal and a cleaner-defer hook.
        run_case "$lane" admission-batch-drain python3 \
            "$REPO/tests/unit_cc/paged_admission_batch_drain.py" \
            "$PORT" "$LOG_DIR"
    fi
    # The dark-feature rehearsal restarts this deployment with conversion
    # disabled, so it must be the final scenario in the lane.
    if tier_at_least nightly; then
        run_case "$lane" dark-feature-restart python3 \
            "$REPO/tests/unit_cc/paged_dark_feature.py" \
            "$START_SCRIPT" "$START_DARK_SCRIPT"
    fi
    "$REPO/tests/unit_cc/paged_single_node.sh" down
    ACTIVE_HARNESS=""
}

run_cluster_lane() {
    local lane="$1" wal=off
    [ "$lane" = P1 ] && wal=on
    ACTIVE_HARNESS=cluster
    "$REPO/tests/unit_cc/standby_cluster.sh" up --wal "$wal" \
        --tag "${lane,,}-$SEED-$(date +%H%M%S)"
    local leader replica log_dir leader_script replica_script
    leader="$(python3 "$REPO/tests/unit_cc/paged_find_leader.py" 7401 7501)"
    if [ "$leader" = 7401 ]; then
        replica=7501
        log_dir="$REPO/bld-standby/p_log"
        leader_script="$REPO/bld-standby/start_p.sh"
        replica_script="$REPO/bld-standby/start_s.sh"
    else
        replica=7401
        log_dir="$REPO/bld-standby/s_log"
        leader_script="$REPO/bld-standby/start_s.sh"
        replica_script="$REPO/bld-standby/start_p.sh"
    fi
    run_common "$lane" "$leader" "$log_dir"
    if tier_at_least pr; then
        run_case "$lane" lifecycle python3 \
            "$REPO/tests/unit_cc/paged_lifecycle_matrix.py" \
            --mode cluster --port "$leader" --log-dir "$log_dir" \
            --start-script "$leader_script" --replica-port "$replica" \
            --replica-start-script "$replica_script" --checkpoint-wait 15
    fi
    run_case_inconclusive "$lane" standby-apply python3 "$REPO/tests/unit_cc/standby_paged_apply.py" \
        "$REPO/bld-standby/start_p.sh" "$REPO/bld-standby/start_s.sh" 500 80
    if tier_at_least pr; then
        run_case "$lane" standby-conversion python3 \
            "$REPO/tests/unit_cc/standby_conversion_paths.py" \
            "$leader" "$replica"
        run_case "$lane" graceful-failover python3 \
            "$REPO/tests/unit_cc/standby_failover_paged.py" \
            "$REPO/bld-standby/start_p.sh" "$REPO/bld-standby/start_s.sh"
        # The failover scenario changes roles and then fully restarts both
        # nodes.  Rediscover instead of passing the lane's pre-failover role
        # assignment to TTL, soak, and memory tests; the election may choose
        # either node, and the other listener may still be coming up.
        leader="$(python3 "$REPO/tests/unit_cc/paged_find_leader.py" 7401 7501)"
        if [ "$leader" = 7401 ]; then
            replica=7501
            log_dir="$REPO/bld-standby/p_log"
            leader_script="$REPO/bld-standby/start_p.sh"
            replica_script="$REPO/bld-standby/start_s.sh"
        else
            replica=7401
            log_dir="$REPO/bld-standby/s_log"
            leader_script="$REPO/bld-standby/start_s.sh"
            replica_script="$REPO/bld-standby/start_p.sh"
        fi
    fi
    # Run last: a failure in one representation transition must not prevent
    # the non-TTL apply, conversion, and failover coverage above from running.
    run_case "$lane" standby-ttl python3 \
        "$REPO/tests/unit_cc/paged_standby_ttl.py" "$leader" "$replica"
    if tier_at_least nightly; then
        local soak_steps=5000
        [ "$TIER" = release ] && soak_steps=50000
        run_case "$lane" soak python3 "$REPO/tests/unit_cc/paged_soak.py" \
            --port "$leader" --log-dir "$log_dir" --seed "$SEED" \
            --steps "$soak_steps" --checkpoint-wait 8
    fi
    # Same isolation rule as the single-node lane: admission-refusal is last.
    if tier_at_least pr; then
        run_case "$lane" memory-pressure python3 \
            "$REPO/tests/unit_cc/paged_memory_pressure_matrix.py" \
            --port "$leader" --log-dir "$log_dir" --checkpoint-wait 8
    fi
    "$REPO/tests/unit_cc/standby_cluster.sh" down
    ACTIVE_HARNESS=""
}

run_ttl_conversion_replay() {
    local special_root="$REPO/bld-paged-matrix/ttl-conversion"
    ELOQKV_PAGED_SINGLE_ROOT="$special_root" \
    ELOQKV_PAGED_SINGLE_PORT=7399 \
    ELOQKV_PAGED_LOGSV_PORT=19091 \
    ELOQKV_PAGED_THRESHOLD=4096 \
    ELOQKV_PAGED_PAGE_SIZE=131072 \
        "$REPO/tests/unit_cc/paged_single_node.sh" up --wal on \
        --tag "ttl-conversion-$SEED-$(date +%H%M%S)"
    # shellcheck disable=SC1090
    source "$special_root/current.env"
    run_case S1 ttl-conversion-replay python3 \
        "$REPO/tests/unit_cc/paged_ttl_convert_txn.py" "$START_SCRIPT" "$PORT"
    ELOQKV_PAGED_SINGLE_ROOT="$special_root" \
    ELOQKV_PAGED_SINGLE_PORT=7399 \
        "$REPO/tests/unit_cc/paged_single_node.sh" down
}

run_production_config() {
    local special_root="$REPO/bld-paged-matrix/production-config"
    ACTIVE_HARNESS=production
    ELOQKV_PAGED_SINGLE_ROOT="$special_root" \
    ELOQKV_PAGED_SINGLE_PORT=7398 \
    ELOQKV_PAGED_LOGSV_PORT=19098 \
    ELOQKV_PAGED_THRESHOLD=4194304 \
    ELOQKV_PAGED_PAGE_SIZE=131072 \
    ELOQKV_PAGED_NODE_MEMORY_MB=384 \
        "$REPO/tests/unit_cc/paged_single_node.sh" up --wal off \
        --tag "production-config-$SEED-$(date +%H%M%S)"
    # shellcheck disable=SC1090
    source "$special_root/current.env"
    run_case S0 production-config python3 \
        "$REPO/tests/unit_cc/paged_production_config.py" \
        --port "$PORT" --log-dir "$LOG_DIR" --start-script "$START_SCRIPT" \
        --threshold "$PAGED_THRESHOLD" --checkpoint-wait 15
    ELOQKV_PAGED_SINGLE_ROOT="$special_root" \
    ELOQKV_PAGED_SINGLE_PORT=7398 \
        "$REPO/tests/unit_cc/paged_single_node.sh" down
    ACTIVE_HARNESS=""
}

run_units
for ((iteration=1; iteration<=REPEAT; ++iteration)); do
    for candidate in S0 S1 P0 P1; do
        if [ "$LANE" != all ] && [ "$LANE" != "$candidate" ]; then
            continue
        fi
        case "$candidate" in
            S0|S1) run_single_lane "$candidate" ;;
            P0|P1) run_cluster_lane "$candidate" ;;
        esac
    done
done

if tier_at_least nightly && { [ "$LANE" = S1 ] || [ "$LANE" = all ]; }; then
    run_ttl_conversion_replay
fi
if [ "$TIER" = release ] && { [ "$LANE" = S0 ] || [ "$LANE" = all ]; }; then
    run_production_config
fi

echo "results: $RESULTS"
if [ "$INCONCLUSIVE" -ne 0 ] && [ "$TIER" != release ]; then
    echo "one or more scenarios were inconclusive (recorded in results.tsv)"
fi
exit "$FAILED"
