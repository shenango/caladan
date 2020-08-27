#!/bin/sh

set -e

CORES=`getconf _NPROCESSORS_ONLN`

# Initialize submodules
git submodule init
git submodule update --init -f --recursive

echo building DPDK
patch -p 1 -d dpdk/ < build/ixgbe_19_11.patch
if lspci | grep -q 'ConnectX-[4,5]'; then
  patch -p 1 -d dpdk/ < build/mlx5_19_11.patch
elif lspci | grep -q 'ConnectX-3'; then
  patch -p 1 -d dpdk/ < build/mlx4_19_11.patch
fi
make -C dpdk/ config T=x86_64-native-linuxapp-gcc
make -C dpdk/ -j $CORES

echo building SPDK
cd spdk
git apply ../build/spdk.patch
./configure
make -j $CORES
cd ..

echo building RDMA-CORE
cd rdma-core
git apply ../build/rdma-core.patch
EXTRA_CMAKE_FLAGS=-DENABLE_STATIC=1 MAKEFLAGS=-j$CORES ./build.sh
cd ..
