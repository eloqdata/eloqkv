#!/usr/bin/env bash

set -e
DIR=$(realpath $0) && DIR=${DIR%/*}
cd $DIR/..
set -x

git submodule update --init --recursive
docker build -f $DIR/docker/eloqkv.docker .
