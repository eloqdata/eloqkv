#!/usr/bin/env bash

set -e
DIR=$(realpath $0) && DIR=${DIR%/*}
cd $DIR/..
set -x

mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release $@ ..
make -j$(nproc)
