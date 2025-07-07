set -xe

redis_branch_name=${1}
tx_service_branch_name=${2}
log_service_branch_name=${3}
eloq_metrics_branch_name=${4}
waiter_branch_name=${5}
redis_server_name=${6}
log_server_name=${7}
rest_api_name=${8}
mono_cluster_mgr_name=${9}
host_manager_branch_name=${10}
defult_branch_name="main"

if [ -z "$1" ]; then
    redis_branch_name=${defult_branch_name}
fi
if [ -z "$2" ]; then
    tx_service_branch_name=${defult_branch_name}
fi
if [ -z "$3" ]; then
    log_service_branch_name=${defult_branch_name}
fi
if [ -z "$4" ]; then
    eloq_metrics_branch_name=${defult_branch_name}
fi
if [ -z "$5" ]; then
    waiter_branch_name=${defult_branch_name}
fi
if [ -z "$6" ]; then
    redis_server_name="eloqkv_arm.tar.gz"
fi
if [ -z "$7" ]; then
    log_server_name="log_service_arm.tar.gz"
fi
if [ -z "$8" ]; then
    rest_api_name="mono_cluster_mgr_rest_dest_arm.tar.gz"
fi
if [ -z "$9" ]; then
    mono_cluster_mgr_name="mono_cluster_mgr_arm.tar.gz"
fi
if [ -z "${10}" ]; then
    host_manager_branch_name=${defult_branch_name}
fi

clone_repo_with_branch() {
    local repo_url="$1"
    local branch_name="$2"
    local file_name="$3"

    if git ls-remote --exit-code --heads "$repo_url" "$branch_name" >/dev/null 2>&1; then
        echo "Cloning repository and checking out branch..."
        git clone -b "$branch_name" "$repo_url" "$file_name"
        cd ${file_name}
    else
        echo "Error: Branch '$branch_name' does not exist in the repository."
        exit 1
    fi
}

copy_libraries() {
    local executable="$1"
    local path="$2"

    libraries=$(ldd "$executable" | awk '/=>/ {print $(NF-1)}')

    if [ ! -e "$file_path" ]; then
        mkdir -p "$path"
    fi

    for lib in $libraries; do
        sudo cp "$lib" "$path/"
    done
}

workspace=${PWD}
build_file_path="${workspace}/build"
echo "build_file_path = ${build_file_path}"
clean_up() {
    echo "clean up"
    cd ${workspace}
    rm -rf ${build_file_path}
}

CFLAGS="-O3 -g -fno-omit-frame-pointer -fno-strict-aliasing -DNDEBUG -DDBUG_OFF"
CXXFLAGS="$CFLAGS -felide-constructors -Wno-error"
CMAKE_FEATURE_OPTS="-DWITH_READLINE=1"
CMAKE_BUILD_OPTS="-DCMAKE_BUILD_TYPE=RelWithDebInfo"

trap clean_up EXIT
#clean_up

mkdir -p ${build_file_path}
cd ${build_file_path}

clone_repo_with_branch git@github.com:eloqdata/eloqkv.git ${redis_branch_name} eloqkv
git submodule update --init --recursive
git clone https://github.com/eloqdata/cpp-driver.git cass
redis_file_path=${PWD}

clone_repo_with_branch git@github.com:eloqdata/tx_service.git ${tx_service_branch_name} tx_service
clone_repo_with_branch git@github.com:monographdb/host_manager.git ${host_manager_branch_name} host_manager
cd ..
clone_repo_with_branch git@github.com:monographdb/log_service.git ${log_service_branch_name} log_service
clone_repo_with_branch git@github.com:eloqdata/eloq-metrics.git ${eloq_metrics_branch_name} eloq_metrics

cd ${build_file_path}/eloqkv
mkdir build
cd build

/usr/bin/cmake ${CMAKE_BUILD_OPTS} ..
/usr/bin/cmake --build . --target eloqkv -- -j8
/usr/bin/cmake --build . --target eloqkv-client -- -j8
/usr/bin/cmake --build . --target host_manager -- -j8
copy_libraries ./eloqkv-client ${redis_file_path}/install/lib
copy_libraries ./eloqkv ${redis_file_path}/install/lib
copy_libraries ./host_manager ${redis_file_path}/install/lib
mv ./eloqkv ${redis_file_path}/install
mv ./eloqkv-client ${redis_file_path}/install
mv ./host_manager ${redis_file_path}/install

cd ${redis_file_path}/tx_service/log_service
mkdir logserver
mkdir build
cd logserver
mkdir bin
cd ../
cd build
/usr/bin/cmake ${CMAKE_BUILD_OPTS} ..

# build and copy log_server
/usr/bin/cmake --build ${redis_file_path}/tx_service/log_service/build -- -j8
copy_libraries ${redis_file_path}/tx_service/log_service/build/launch_sv ${redis_file_path}/tx_service/log_service/logserver/lib
mv ${redis_file_path}/tx_service/log_service/build/launch_sv ${redis_file_path}/tx_service/log_service/logserver/bin

cd ${build_file_path}
clone_repo_with_branch git@github.com:monographdb/monograph_waiter.git ${waiter_branch_name} monograph_waiter
cargo make --no-workspace --makefile Makefile.toml rest_api_pkg
cargo make --no-workspace --makefile Makefile.toml cluster_mgr_pkg

cd ${build_file_path}
tar -czvf eloqkv_arm.tar.gz -C ${redis_file_path} install
tar -czvf log_service_arm.tar.gz -C ${redis_file_path}/tx_service/log_service logserver
tar -czvf mono_cluster_mgr_rest_dest_arm.tar.gz -C ${build_file_path}/monograph_waiter/ mono_cluster_mgr_rest_dest
tar -czvf mono_cluster_mgr_arm.tar.gz -C ${build_file_path}/monograph_waiter/ mono_cluster_mgr_dist

aws s3 cp eloqkv_arm.tar.gz s3://monographdb-release/mono-release-ci-redis/${redis_server_name}
aws s3 cp log_service_arm.tar.gz s3://monographdb-release/mono-release-ci-redis/${log_server_name}
aws s3 cp mono_cluster_mgr_rest_dest_arm.tar.gz s3://monographdb-release/mono-release-ci-redis/${rest_api_name}
aws s3 cp mono_cluster_mgr_arm.tar.gz s3://monographdb-release/mono-release-ci-redis/${mono_cluster_mgr_name}
