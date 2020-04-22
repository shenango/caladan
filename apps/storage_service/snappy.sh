#!/bin/bash

set -e

git submodule update --init --recursive

pushd snappy
rm -rf build
mkdir build
pushd build

cmake -DSNAPPY_BUILD_TESTS=0 -DCMAKE_BUILD_TYPE=Release ..
make -j

popd
popd
