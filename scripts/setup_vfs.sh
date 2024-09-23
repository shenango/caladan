#!/bin/bash

set -e
#set -x

# set these 3 variables
IF=$1
NR_VFS=2

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
DPDK_PATH=$SCRIPT_DIR/../dpdk

IFPCI=$(basename $(readlink /sys/class/net/$IF/device))
MAX_VF=$NR_VFS
MAX_VF_INDEX=$((MAX_VF - 1))

echo "Setting up $IF with VFs"

echo "Removing existing vfs..."
echo 0 | sudo tee  /sys/class/net/$IF/device/mlx5_num_vfs > /dev/null || true
echo "Reloading mellanox drivers..."
sudo /etc/init.d/openibd restart
sudo ip link set down $IF  || true

echo "Creating new VFs"
echo $MAX_VF | sudo tee /sys/class/net/$IF/device/mlx5_num_vfs > /dev/null

for i in `seq 0 $MAX_VF_INDEX`; do
	sudo ip link set $IF vf $i mac 02:02:02:02:02:$(printf "%02d\n" $(($i + 5)))
done

echo "unbinding new VFs from mlx5_core"
for i in `seq 0 $MAX_VF_INDEX`; do
	pfx=$(basename $(readlink /sys/class/net/$IF/device/virtfn${i}))
	echo $pfx | sudo tee /sys/bus/pci/drivers/mlx5_core/unbind > /dev/null
done

echo "Set up switchdev"
sudo devlink dev eswitch set pci/$IFPCI mode switchdev

echo "binding VF for vfio"
sudo modprobe vfio_pci
sudo $DPDK_PATH/usertools/dpdk-devbind.py -b vfio-pci $(basename $(readlink /sys/class/net/$IF/device/virtfn0))

echo "rebinding remaining VFs for linux"
for i in `seq 1 $MAX_VF_INDEX`; do
	pfx=$(basename $(readlink /sys/class/net/$IF/device/virtfn${i}))
	sudo $DPDK_PATH/usertools/dpdk-devbind.py -b mlx5_core $pfx
done

echo "setting up switch rules"
sudo ethtool -K $IF hw-tc-offload on

REPS=()
for f in `ls /sys/class/net/$IF/device/net | grep -v $IF`; do
	REPS+=($f)
done

for i in `seq 0 $MAX_VF_INDEX`; do
	sudo ethtool -K ${REPS[$i]} hw-tc-offload on
	sudo ip link set up ${REPS[$i]}
done

sudo tc qdisc add dev $IF ingress
for i in `seq 0 $MAX_VF_INDEX`; do
	sudo tc qdisc add dev ${REPS[$i]} ingress
done

for vfA in `seq 0  $MAX_VF_INDEX`; do

	# REDIRECT: unicast ingress traffic from outside this host with vfA's mac address
	sudo tc filter add dev $IF protocol all parent ffff: prio 1 flower dst_mac 02:02:02:02:02:0$((5 + $vfA)) action mirred egress redirect dev ${REPS[$vfA]}
	# MIRROR: broadcast ARP traffic
	sudo tc filter add dev $IF protocol arp parent ffff: prio $((3 + $vfA)) flower dst_mac ff:ff:ff:ff:ff:ff action mirred egress mirror dev ${REPS[$vfA]}

	for vfB in `seq $(($vfA + 1)) $MAX_VF_INDEX`; do
		# REDIRECT: pairwise unicast traffic between vfA and vfB
		sudo tc filter add dev ${REPS[$vfA]} protocol all parent ffff: prio 1 flower dst_mac 02:02:02:02:02:0$((5 + $vfB)) action mirred egress redirect dev ${REPS[$vfB]}
		sudo tc filter add dev ${REPS[$vfB]} protocol all parent ffff: prio 1 flower dst_mac 02:02:02:02:02:0$((5 + $vfA)) action mirred egress redirect dev ${REPS[$vfA]}

		# MIRROR: pairise broadcast traffic between vfA and vfB
		sudo tc filter add dev ${REPS[$vfB]} protocol arp parent ffff: prio 3 flower dst_mac ff:ff:ff:ff:ff:ff action mirred egress mirror dev ${REPS[$vfA]}
		sudo tc filter add dev ${REPS[$vfA]} protocol arp parent ffff: prio 3 flower dst_mac ff:ff:ff:ff:ff:ff action mirred egress mirror dev ${REPS[$vfB]}
	done

	# REDIRECT: remaining egress traffic out from this host
	sudo tc filter add dev ${REPS[$vfA]} protocol all parent ffff: prio 100 flower action mirred egress redirect dev $IF

done

LNX=$(ls /sys/class/net/$IF/device/virtfn1/net)

sudo ip link set up $LNX

echo "All done!"
echo "VFIO pci is $(basename $(readlink /sys/class/net/$IF/device/virtfn0))"
echo "Linux interface is $LNX"
