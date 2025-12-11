#!/bin/bash
set -exo pipefail

export WORKSPACE=$PWD
export AWS_PAGER=""
current_user=$(whoami)
sudo chown -R $current_user $PWD
cd $HOME

mkdir -p ~/.ssh
echo "$GIT_SSH_KEY" > ~/.ssh/id_rsa
chmod 600 ~/.ssh/id_rsa
ssh-keyscan github.com >> ~/.ssh/known_hosts

ln -s ${WORKSPACE}/eloqkv_src eloqkv
cd eloqkv
# ensure log service under expected path
if [ -d "$WORKSPACE/logservice_src" ]; then
  ln -s $WORKSPACE/logservice_src data_substrate/eloq_log_service
fi
pushd data_substrate/tx_service
ln -s $WORKSPACE/raft_host_manager_src raft_host_manager
popd

if [ "${DATA_STORE_TYPE}" = "ELOQDSS_ELOQSTORE" ]; then
    pushd data_substrate/store_handler/eloq_data_store_service
    ln -s $WORKSPACE/eloqstore_src eloqstore
    popd
fi
ELOQKV_SRC=${PWD}

# Get OS information from /etc/os-release
source /etc/os-release
if [[ "$ID" == "centos" ]] || [[ "$ID" == "rocky" ]]; then
    OS_ID="rhel${VERSION_ID%.*}"
else
    OS_ID="${ID}${VERSION_ID%.*}"
fi
if [[ "$OS_ID" == rhel* ]]; then
    case "$VERSION_ID" in
    7*)
        sudo yum update -y
        sudo yum install rsync -y
        source /opt/rh/devtoolset-11/enable
        g++ --version
        INSTALL_PSQL="sudo yum install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-7-x86_64/pgdg-redhat-repo-latest.noarch.rpm && sudo yum install -y postgresql14"
        ;;
    8*)
        sudo dnf update -y
        sudo dnf install rsync -y
        source scl_source enable gcc-toolset-11
        g++ --version
        INSTALL_PSQL="sudo dnf install -y postgresql"
        ;;
    9*)
        sudo dnf update -y
        sudo dnf install rsync -y
        INSTALL_PSQL="sudo dnf install -y postgresql"
        # detected dubious ownership
        git config --global --add safe.directory ${WORKSPACE}/eloqkv_src
        git config --global --add safe.directory ${WORKSPACE}/logservice_src
        git config --global --add safe.directory ${WORKSPACE}/raft_host_manager_src
        ;;
    esac
elif [[ "$OS_ID" == ubuntu* ]]; then
    sudo apt update -y
    sudo apt install rsync -y
    INSTALL_PSQL="DEBIAN_FRONTEND=noninteractive sudo apt install -y postgresql-client"
fi

case $(uname -m) in
amd64 | x86_64) ARCH=amd64 ;;
arm64 | aarch64) ARCH=arm64 ;;
*) ARCH= $(uname -m) ;;
esac

if [ -n "${TAGGED}" ]; then
    TAGGED=$(git tag --sort=-v:refname | head -n 1)
    if [ -z "${TAGGED}" ]; then
        exit 1
    fi
    scripts/git-checkout.sh "${TAGGED}"
fi

copy_libraries() {
    local executable="$1"
    local path="$2"
    libraries=$(ldd "$executable" | awk 'NF==4{print $(NF-1)}{}')
    local -a skip_patterns=(
        "libc.so*"
        "libm.so*"
        "libpthread.so*"
        "libdl.so*"
        "librt.so*"
        "libanl.so*"
        "libresolv.so*"
        "libutil.so*"
        "libnsl.so*"
        "ld-linux*.so*"
    )
    mkdir -p "$path"
    local lib
    for lib in $libraries; do
        if [[ ! -e "$lib" ]]; then
            continue
        fi
        local lib_name
        lib_name=$(basename "$lib")
        local skip_lib=false
        for pattern in "${skip_patterns[@]}"; do
            if [[ "$lib_name" == $pattern ]]; then
                skip_lib=true
                break
            fi
        done
        if [[ "$skip_lib" == true ]]; then
            continue
        fi
        rsync -avL --ignore-existing "$lib" "$path/"
    done
}

S3_BUCKET="eloq-release"
S3_PREFIX="s3://${S3_BUCKET}/eloqkv"

# Normalize behavior for supported DATA_STORE_TYPE values
if [ "${DATA_STORE_TYPE}" = "ROCKSDB" ]; then
    DATA_STORE_ID="rocksdb"
elif [ "${DATA_STORE_TYPE}" = "ELOQDSS_ROCKSDB_CLOUD_S3" ]; then
    CMAKE_ARGS="${CMAKE_ARGS} -DWITH_LOG_STATE=ROCKSDB_CLOUD_S3 -DWITH_CLOUD_AZ_INFO=ON"
    DATA_STORE_ID="rocks_s3"
elif [ "${DATA_STORE_TYPE}" = "ELOQDSS_ROCKSDB_CLOUD_GCS" ]; then
    CMAKE_ARGS="${CMAKE_ARGS} -DWITH_LOG_STATE=ROCKSDB_CLOUD_GCS"
    DATA_STORE_ID="rocks_gcs"
elif [ "${DATA_STORE_TYPE}" = "ELOQDSS_ROCKSDB" ]; then
    DATA_STORE_ID="eloqdss_rocksdb"
elif [ "${DATA_STORE_TYPE}" = "ELOQDSS_ELOQSTORE" ]; then
    DATA_STORE_ID="eloqdss_eloqstore"
else
    echo "Unsupported DATA_STORE_TYPE: ${DATA_STORE_TYPE}"
    exit 1
fi


if [ "$ASAN" = "ON" ]; then
    export ASAN_OPTIONS=abort_on_error=1:detect_container_overflow=0:leak_check_at_exit=0
fi

# init destination directory
DEST_DIR="${HOME}/EloqKV"
mkdir ${DEST_DIR}
mkdir ${DEST_DIR}/bin
mkdir ${DEST_DIR}/lib
mkdir ${DEST_DIR}/conf

# Define the license content for tarball
LICENSE_CONTENT=$(
    cat <<EOF
License

Copyright (c) 2024 EloqData

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to use,
copy, modify, and distribute the Software, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT. IN NO EVENT SHALL ELOQDATA
OR ITS CONTRIBUTORS BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT, OR OTHERWISE, ARISING FROM, OUT OF, OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

IMPORTANT: By using this software, you acknowledge that EloqData shall not be
liable for any loss or damage, including but not limited to loss of data, arising
from the use of the software. The responsibility for backing up any data, checking
the software's appropriateness for your needs, and using it within the bounds of
the law lies entirely with you.
EOF
)

# Write the license content to LICENSE.txt in the destination directory
echo "$LICENSE_CONTENT" >"${DEST_DIR}/LICENSE.txt"

# build redis-cli
CLIENT_S3_FILE="client/eloqkv-cli-7.2.5-${OS_ID}-${ARCH}"
aws s3api head-object --bucket ${S3_BUCKET} --key eloqkv/${CLIENT_S3_FILE}
aws s3 cp ${S3_PREFIX}/${CLIENT_S3_FILE} redis-cli
chmod +x redis-cli
mv redis-cli ${DEST_DIR}/bin/eloqkv-cli

# build eloqkv
cd $ELOQKV_SRC
mkdir build && cd build

BUILD_TYPE_NORMALIZED=$(echo "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')
# Keep checkpoint reporting and code line trimming enabled for Debug builds.
CKPT_REPORT_FLAG="ON"
CODE_LINE_FLAG="ON"
if [[ "${BUILD_TYPE_NORMALIZED}" == "debug" ]]; then
    CKPT_REPORT_FLAG="OFF"
    CODE_LINE_FLAG="OFF"
fi

cmake .. -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DWITH_DATA_STORE=$DATA_STORE_TYPE $CMAKE_ARGS \
    -DWITH_LOG_SERVICE=ON -DDISABLE_CKPT_REPORT=${CKPT_REPORT_FLAG} -DDISABLE_CODE_LINE_IN_LOG=${CODE_LINE_FLAG} \
    -DWITH_ASAN=$ASAN -DOPEN_LOG_SERVICE=OFF -DFORK_HM_PROCESS=ON
cmake --build . --config ${BUILD_TYPE} -j${NCORE}
copy_libraries eloqkv ${DEST_DIR}/lib
mv eloqkv ${DEST_DIR}/bin/
copy_libraries data_substrate/host_manager ${DEST_DIR}/lib
mv data_substrate/host_manager ${DEST_DIR}/bin/

# build dss_server and include in tarball
# Map DATA_STORE_TYPE to DSS-compatible values (any ELOQDSS_* builds DSS; others skip)
if [[ "${DATA_STORE_TYPE}" == ELOQDSS_* ]]; then
    DSS_TYPE="${DATA_STORE_TYPE}"
else
    DSS_TYPE=""
fi

if [ -n "${DSS_TYPE}" ]; then
    DSS_SRC_DIR="${ELOQKV_SRC}/data_substrate/store_handler/eloq_data_store_service"
    cd "${DSS_SRC_DIR}"

    if [ "${DATA_STORE_TYPE}" = "ELOQDSS_ELOQSTORE" ]; then
        DSS_CMAKE_ARGS="${DSS_CMAKE_ARGS} -DELOQ_MODULE_ENABLED=ON"
    fi

    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DWITH_DATA_STORE=${DSS_TYPE} ${DSS_CMAKE_ARGS}
    cmake --build . --config ${BUILD_TYPE} -j${NCORE}
    copy_libraries dss_server ${DEST_DIR}/lib
    mv dss_server ${DEST_DIR}/bin/
    cd "${ELOQKV_SRC}"
fi

if [ "${DATA_STORE_TYPE}" = "ROCKSDB" ]; then
    copy_libraries eloqkv_to_aof ${DEST_DIR}/lib
    mv eloqkv_to_aof ${DEST_DIR}/bin/
    copy_libraries eloqkv_to_rdb ${DEST_DIR}/lib
    mv eloqkv_to_rdb ${DEST_DIR}/bin/
fi

# set rpath for prebuild shared library
patchelf --set-rpath '$ORIGIN' ${DEST_DIR}/lib/libleveldb.*
patchelf --set-rpath '$ORIGIN' ${DEST_DIR}/lib/libbrpc.*
patchelf --set-rpath '$ORIGIN' ${DEST_DIR}/lib/libbraft.*
patchelf --set-rpath '$ORIGIN' ${DEST_DIR}/lib/librocksdb*

cp ${ELOQKV_SRC}/concourse/artifact/eloqkv.ini ${DEST_DIR}/conf/
tar -czvf eloqkv.tar.gz -C ${HOME} EloqKV

if [ -n "${TAGGED}" ]; then
    TX_TARBALL="eloqkv-${TAGGED}-${OS_ID}-${ARCH}.tar.gz"
    eval ${INSTALL_PSQL}
    SQL="INSERT INTO tx_release VALUES ('eloqkv', '${ARCH}', '${OS_ID}', '${DATA_STORE_ID}', $(echo ${TAGGED} | tr '.' ',')) ON CONFLICT DO NOTHING"
    psql postgresql://${PG_CONN}/eloq_release?sslmode=require -c "${SQL}"
else
    TX_TARBALL="eloqkv-${OUT_NAME}-${OS_ID}-${ARCH}.tar.gz"
fi
aws s3 cp eloqkv.tar.gz ${S3_PREFIX}/${DATA_STORE_ID}/${TX_TARBALL}
if [ -n "${CLOUDFRONT_DIST}" ]; then
    aws cloudfront create-invalidation --distribution-id ${CLOUDFRONT_DIST} --paths "/eloqkv/${DATA_STORE_ID}/${TX_TARBALL}"
fi

# clean up eloqkv tx build artifacts
rm -rf eloqkv.tar.gz
cd $ELOQKV_SRC
rm -rf build
rm -rf ${DEST_DIR}

build_upload_log_srv() {
    if [ "$#" -lt 2 ]; then
      echo "Error: Function build_upload_log_srv requires at least 2 parameters."
      exit 1
    fi
    local log_tarball=$1
    local kv_type=$2
    log_sv_src=${ELOQKV_SRC}/data_substrate/log_service
    cd ${log_sv_src}
    mkdir -p LogService/bin
    mkdir build && cd build
    cmake_args="-DCMAKE_BUILD_TYPE=$BUILD_TYPE -DWITH_ASAN=$ASAN -DDISABLE_CODE_LINE_IN_LOG=ON"
    if [ "$kv_type" = "ELOQDSS_ROCKSDB_CLOUD_S3" ]; then
        cmake_args="$cmake_args -DWITH_LOG_STATE=ROCKSDB_CLOUD_S3 -DWITH_CLOUD_AZ_INFO=ON"
    elif [ "$kv_type" = "ELOQDSS_ROCKSDB_CLOUD_GCS" ]; then
        cmake_args="$cmake_args -DWITH_LOG_STATE=ROCKSDB_CLOUD_GCS"
    fi
    cmake .. $cmake_args
    # build and copy log_server
    cmake --build . --config $BUILD_TYPE -j${NCORE}
    mv ${log_sv_src}/build/launch_sv ${log_sv_src}/LogService/bin
    copy_libraries ${log_sv_src}/LogService/bin/launch_sv ${log_sv_src}/LogService/lib
    cd ${HOME}
    tar -czvf log_service.tar.gz -C ${log_sv_src} LogService
    aws s3 cp log_service.tar.gz ${S3_PREFIX}/logservice/${DATA_STORE_ID}/${log_tarball}
    #clean up
    rm -rf log_service.tar.gz
    cd "${log_sv_src}"
    rm -rf build
    rm -rf LogService
 }

if [ "${BUILD_LOG_SRV}" = true ]; then
    # make and build log_service
    if [ -n "${TAGGED}" ]; then
        LOG_TARBALL="log-service-${TAGGED}-${OS_ID}-${ARCH}.tar.gz"
    else
        LOG_TARBALL="log-service-${OUT_NAME}-${OS_ID}-${ARCH}.tar.gz"
    fi

    build_upload_log_srv "${LOG_TARBALL}" "${DATA_STORE_TYPE}"

    if [ -n "${CLOUDFRONT_DIST}" ]; then
        aws cloudfront create-invalidation --distribution-id ${CLOUDFRONT_DIST} --paths "/eloqkv/logservice/${DATA_STORE_ID}/${LOG_TARBALL}"
    fi
fi
