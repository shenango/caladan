#!/bin/bash

set -e

git submodule update --init --recursive

pushd rdma-core
git apply ../rdma-core.patch || true

EXTRA_CMAKE_FLAGS=-DENABLE_STATIC=1 MAKEFLAGS=-j ./build.sh

popd
