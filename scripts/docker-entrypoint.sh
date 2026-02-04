#!/bin/bash
set -eu

cd ${HOME}/EloqKV
if [ ! -f eloqkv.ini ]; then
  # update local IP for the first time to run
  ln -s conf/eloqkv.ini eloqkv.ini
  # Get container IP more robustly
  MY_IP=$(hostname -I | awk '{print $1}')
  if [ -z "${MY_IP}" ]; then
    MY_IP=$(ip -4 addr | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | head -n 1)
  fi
  sed -i "s|127.0.0.1|${MY_IP}|g" eloqkv.ini
fi
mkdir -p logs
export GLOG_log_dir=logs
export GLOG_max_log_size=1024
./bin/eloqkv --config eloqkv.ini
