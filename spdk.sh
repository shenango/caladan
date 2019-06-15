#!/bin/bash

set -e

git submodule update --init --recursive
patch -p 1 -d spdk < spdk.patch
cd spdk
./configure
make
sudo scripts/setup.sh
