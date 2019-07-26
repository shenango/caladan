#!/bin/bash

set -e

git submodule update --init --recursive

pushd spdk
git apply ../spdk.patch || true
./configure
make -j
sudo scripts/setup.sh
popd
