#!/usr/bin/env bash

set -e
DIR=$(realpath $0) && DIR=${DIR%/*}
cd $DIR/..
set -x

echo "Starting eloqkv server in background..."
./build/eloqkv \
  --port=6379 \
  --core_number=2 \
  --enable_wal=false \
  --enable_data_store=false \
  --enable_io_uring=false \
  --logtostderr=true >/tmp/eloqkv.log 2>&1 &
SERVER_PID=$!

cleanup() {
  echo "Stopping eloqkv server (PID: $SERVER_PID)..."
  kill $SERVER_PID || true
  wait $SERVER_PID 2>/dev/null || true
  echo "Server stopped."
}
trap cleanup EXIT

echo "Waiting for eloqkv to be ready..."
for i in {1..20}; do
  if (echo -en "PING\r\n" | nc 127.0.0.1 6379 | grep -q +PONG); then
    echo "eloqkv server is ready!"
    break
  fi
  sleep 0.5
done

echo "Running Tcl tests..."
TEST_FILES=$(find tests/unit/eloq -maxdepth 1 -name "*.tcl" | sort)
FAILED_TESTS=""

for file in $TEST_FILES; do
  test_name=$(basename "$file" .tcl)
  echo "Running test: $test_name"

  if ! tclsh tests/test_helper.tcl \
    --host 127.0.0.1 \
    --port 6379 \
    --tags -needs:repl \
    --tags -needs:config-maxmemory \
    --tags -needs:debug \
    --tags -needs:redis_config \
    --tags -needs:redis_expire \
    --tags -needs:slow_test \
    --tags -needs:support_cmd_later \
    --tags -needs:fault_inject \
    --tags -needs:no_evicted \
    --single "/unit/eloq/$test_name"; then
    echo "TEST FAILED: $test_name"
    FAILED_TESTS="$FAILED_TESTS $test_name"
  else
    echo "TEST PASSED: $test_name"
  fi
done

if [ -n "$FAILED_TESTS" ]; then
  echo "The following tests failed:$FAILED_TESTS"
  exit 1
else
  echo "All tests passed!"
fi
