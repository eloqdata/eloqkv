#!/usr/bin/env bash

set -e
DIR=$(realpath $0) && DIR=${DIR%/*}
cd $DIR/..
set -x

bash "$DIR/checkout_product_submodules.sh"
docker build -f $DIR/docker/eloqkv.docker .
