#!/bin/bash
# run with sudo

# needed for the iokernel's shared memory
sysctl -w kernel.shm_rmid_forced=1
sysctl -w kernel.shmmax=18446744073692774399
sysctl -w vm.hugetlb_shm_group=27
sysctl -w vm.max_map_count=16777216
sysctl -w net.core.somaxconn=3072

# check to see if we need a fake idle driver
if grep -q none /sys/devices/system/cpu/cpuidle/current_driver; then
  insmod $(dirname $0)/../ksched/build/fake_idle.ko
fi

# set up the ksched module
rmmod ksched
rm /dev/ksched

if [[ "$1x" = "nouintrx" ]]; then
  insmod $(dirname $0)/../ksched/build/ksched.ko nouintr=1
else
  insmod $(dirname $0)/../ksched/build/ksched.ko
fi

mknod /dev/ksched c 280 0
chmod uga+rwx /dev/ksched

# reserve huge pages
for n in /sys/devices/system/node/node*; do
echo 5192 > ${n}/hugepages/hugepages-2048kB/nr_hugepages
done

echo madvise > /sys/kernel/mm/transparent_hugepage/enabled

# load msr module
modprobe msr

