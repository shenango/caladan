#!/bin/sh

set -e

CORES=`getconf _NPROCESSORS_ONLN`

# Initialize submodules
git submodule init
git submodule update --init -f --recursive

echo building RDMA-CORE
cd rdma-core
git apply ../build/rdma-core.patch
EXTRA_CMAKE_FLAGS=-DENABLE_STATIC=1 MAKEFLAGS=-j$CORES ./build.sh
cd ..

echo building DPDK
patch -p 1 -d dpdk/ < build/ixgbe_19_11.patch
if lspci | grep -q 'ConnectX-[4,5]'; then
  rm -f dpdk/drivers/net/mlx5/mlx5_custom.h
  patch -p1 -N -d dpdk/ < build/mlx5_19_11.patch

  # build against local rdma-core library
  export EXTRA_CFLAGS=-I$PWD/rdma-core/build/include
  export EXTRA_LDFLAGS=-L$PWD/rdma-core/build/lib
  export PKG_CONFIG_PATH=$PWD/rdma-core/build/lib/pkgconfig
elif lspci | grep -q 'ConnectX-3'; then
  rm -f dpdk/drivers/net/mlx4/mlx4_custom.h
  patch -p1 -N -d dpdk/ < build/mlx4_19_11.patch
fi
make -C dpdk/ config T=x86_64-native-linuxapp-gcc
make -C dpdk/ -j $CORES

export EXTRA_CFLAGS=
export EXTRA_LDFLAGS=
export PKG_CONFIG_PATH=


echo building SPDK
cd spdk
git apply ../build/spdk.patch
./configure
make -j $CORES
cd ..

echo building PCM
cd deps/pcm
rm -f pcm-caladan.cpp
patch -p1 -N < ../../build/pcm.patch
make lib -j $CORES
cd ../../
