#!/bin/bash
# run with sudo

pushd ..

# Build ksched.ko
cd ksched && make clean && make && cd ..

# Shenango setup
./scripts/setup_machine.sh

# turn on cstate
killall cstate
cd scripts
gcc cstate.c -o cstate
./cstate 0 &
cd ..

# Disable frequency scaling
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Disable turbo boost
echo 1 | tee /sys/devices/system/cpu/intel_pstate/no_turbo

popd
