#!/bin/sh

set -e

CORES=`getconf _NPROCESSORS_ONLN`

if ! hash meson 2> /dev/null; then
  echo "Missing meson. Please install meson!"
  exit 1
fi

# Initialize submodules
git submodule init
git submodule update --init -f --recursive

clean() {
  for mod in dpdk rdma-core spdk deps/pcm; do
    cd $mod
    git checkout .
    git clean -df .
    rm -rf build/
    cd ..
  done
}

if [ "$1" = "clean" ]; then
  clean
  exit 0
fi

echo building RDMA-CORE
cd rdma-core
git -c user.name="x" -c user.email="x" am ../build/patches/rdma-core/*
if ! EXTRA_CMAKE_FLAGS=-DENABLE_STATIC=1 MAKEFLAGS=-j$CORES ./build.sh; then
  echo "Building rdma-core failed"
  echo "If you see \"Does not match the generator used previously\" try running \"make submodules-clean\" first"
  exit 1
fi
cd ..

echo building DPDK

disable_driver='crypto/*,net/bnxt'

export EXTRA_CFLAGS=-I$PWD/rdma-core/build/include
export EXTRA_LDFLAGS=-L$PWD/rdma-core/build/lib
export PKG_CONFIG_PATH=$PWD/rdma-core/build/lib/pkgconfig

#if lspci | grep -q 'ConnectX-3'; then
#  rm -f dpdk/drivers/net/mlx4/mlx4_custom.h
#  patch -p1 -N -d dpdk/ < build/mlx4_22_03.patch
#  disable_driver="${disable_driver},common/mlx5,net/mlx5"
#fi


cd dpdk
git apply ../build/dpdk.patch
meson build
meson configure -Ddisable_drivers=$disable_driver build
meson configure -Dprefix=$PWD/build build
ninja -C build
ninja -C build install
cd ..

export EXTRA_CFLAGS=
export EXTRA_LDFLAGS=
export PKG_CONFIG_PATH=


echo building SPDK
cd spdk
git apply ../build/spdk.patch
git apply ../build/spdk2.patch
./configure --with-dpdk=$PWD/../dpdk/build/
make -j $CORES
cd ..

echo building PCM
cd deps/pcm
rm -f src/pcm-caladan.cpp
patch -p1 -N < ../../build/pcm.patch
mkdir -p build
cd build
cmake ..
make PCM_STATIC -j $CORES
cd ../../../
