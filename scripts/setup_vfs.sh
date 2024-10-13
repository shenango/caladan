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

gen_random_mac() {
    printf '02:%02x:%02x:%02x:%02x:%02x\n' $((RANDOM % 256)) $((RANDOM % 256)) $((RANDOM % 256)) $((RANDOM % 256)) $((RANDOM % 256))
}

mac_array=()
mac_array+=("$(cat /sys/class/net/$IF/address)")

# Generate random macs for each VF
for i in `seq 0 $MAX_VF_INDEX`; do
	mac="$(gen_random_mac)"
	mac_array+=($mac)
	sudo ip link set $IF vf $i mac $mac
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
REPS+=($IF)
for f in `ls /sys/class/net/$IF/device/net | grep -v $IF`; do
	REPS+=($f)
done

for rep in "${REPS[@]}"; do
	sudo ethtool -K ${rep} hw-tc-offload on
	sudo ip link set up ${rep}
	sudo tc qdisc add dev ${rep} ingress
done

sudo ip link set down $IF

ARP_PRIO=$((${#REPS[@]} + 1)) # arp rule goes after unicast rules
EGRESS_PRIO=$(($ARP_PRIO + 1)) # catch-all host egress rule goes after arp rule

UPLINK=$IF

# Setup egress rules for each representor
for rep in "${REPS[@]}"; do

	# Construct ARP broadcast rule from this representor to all others
	tc_command="sudo tc filter add dev ${rep} ingress protocol arp prio ${ARP_PRIO} flower"

	# Configure traffic rules TO all other interfaces
	for j in "${!REPS[@]}"; do
		if [[ "${rep}" == "${REPS[$j]}" ]]; then
			continue
		fi

		# One rule for each other representor matching on its mac address
		sudo tc filter add dev $rep protocol all parent ffff: prio $(($j + 1)) flower dst_mac ${mac_array[$j]} action mirred egress redirect dev ${REPS[$j]}

		# Add other interface to broadcast list for ARP traffic
		tc_command+=" action mirred egress mirror dev ${REPS[$j]}"
	done

	# Install ARP broadcast rule
	eval $tc_command

	# Setup egress from host for all other traffic
	if [[ "$rep" != "$UPLINK" ]]; then
		sudo tc filter add dev $rep protocol all parent ffff: prio ${EGRESS_PRIO} flower action mirred egress redirect dev $UPLINK
	fi
done

LNX=$(ls /sys/class/net/$IF/device/virtfn1/net)

sudo ip link set up $LNX
sudo ip link set up $IF

echo "All done!"
echo "VFIO pci is $(basename $(readlink /sys/class/net/$IF/device/virtfn0))"
echo "Linux interface is $LNX"
