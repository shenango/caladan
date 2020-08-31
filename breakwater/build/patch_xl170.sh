#!/bin/bash

pushd ..
# apply CoonectX-4 patch
git apply breakwater/build/connectx-4.patch
# apply Cloudlab XL170 patch
git apply breakwater/build/cloudlab_xl170.patch
# Re-build Shenango
make clean && make
make -C bindings/cc
popd
