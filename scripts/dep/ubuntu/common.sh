#!/bin/bash

run_with_retry() {
    local max_retries=5
    local attempt=1

    while [ $attempt -le $max_retries ]; do
        echo "Running: $@ (Attempt $attempt/$max_retries)..."
        if "$@"; then
            return 0
        fi
        attempt=$((attempt + 1))
        if [ $attempt -le $max_retries ]; then
            sleep 2
        fi
    done
    return 1
}
