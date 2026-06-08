#!/bin/bash
set -ex

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# Ensure uv is in PATH
export PATH="$HOME/.local/bin:$PATH"

# Install python dependencies using uv
# --system flag can be used if we want to install globally, but usually 
# uv creates its own virtualenv by default or manages 'python' calls.
# Here we follow the user's previous pattern of using a venv at $HOME/venv

# Create and activate venv with uv
uv venv --python /usr/bin/python3 "$HOME/venv"
source "$HOME/venv/bin/activate"

# cassandra-driver 3.28.0 falls back to a legacy sdist build on Python 3.12.
run_with_retry uv pip install --no-cache-dir \
    setuptools==45.2.0 \
    cassandra-driver==3.29.2 \
    awscli==1.29.44 \
    boto3==1.28.36 \
    botocore==1.31.44 \
    mysql-connector-python==8.1.0 \
    psutil==5.9.5 \
    grpcio==1.60.0 \
    grpcio-tools==1.60.0
