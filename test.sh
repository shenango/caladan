#!/bin/bash

set -ex

(sudo pkill iokerneld && sleep 2) || true
sudo pkill -9 netperf || true
sudo scripts/setup_machine.sh ${NOUINTR}
sudo ./iokerneld ias nobw noht no_hw_qdel numanode -1 -- --allow 00:00.0 --vdev=net_tap0 > /tmp/iokernel_$USER.log 2>&1 &


while ! grep -q 'running dataplan' /tmp/iokernel_$USER.log; do
      sleep 0.3
      # make sure it is still alive
      pgrep iokerneld > /dev/null || exit 1
      cat /tmp/iokernel_$USER.log
done

CORES=`getconf _NPROCESSORS_ONLN`

gen_configs() {
    exec {test_fd}<<EOF
        host_addr 192.168.1.5
        host_netmask 255.255.255.0
        host_gateway 192.168.1.1
        runtime_kthreads $((CORES-2))
        runtime_guaranteed_kthreads 0
        runtime_priority lc
EOF

    exec {test_fd2}<<EOF
        host_addr 192.168.1.6
        host_netmask 255.255.255.0
        host_gateway 192.168.1.1
        runtime_kthreads $((CORES-2))
        runtime_guaranteed_kthreads 0
        runtime_priority lc
EOF
}

for test in `find tests -name 'test_*' -executable | grep -v storage`; do
    gen_configs
	$test /proc/self/fd/$test_fd /proc/self/fd/$test_fd2
done

make -C bindings/cc/ -j librt++.a
make -C apps/bench/ netperf -j

gen_configs
apps/bench/netperf /proc/self/fd/$test_fd server &
np=$!
sleep 1

apps/bench/netperf /proc/self/fd/$test_fd2 tcprr 192.168.1.5 10 10000 4096

gen_configs
apps/bench/netperf /proc/self/fd/$test_fd2 tcpstream 192.168.1.5 10 10000 4096

kill $np
