# Caladan

Caladan is a system that enables servers in datacenters to
simultaneously provide low tail latency and high CPU efficiency, by
rapidly reallocating cores across applications.

### Contact

For any questions about Caladan, please email <caladan@csail.mit.edu>.

## How to Run Caladan

1) Clone the Caladan repository.

2) Install dependencies.

```
sudo apt install make gcc cmake pkg-config libnl-3-dev libnl-route-3-dev libnuma-dev uuid-dev libssl-dev libaio-dev libcunit1-dev libclang-dev libncurses-dev meson python3-pyelftools
```

3) Set up submodules (e.g., DPDK, SPDK, and rdma-core).

```
make submodules
```

4) Build the scheduler (IOKernel), the Caladan runtime, and Ksched and perform some machine setup.
Before building, set the parameters in build/config (e.g., `CONFIG_SPDK=y` to use
storage, `CONFIG_DIRECTPATH=y` to use directpath, and the MLX4 or MLX5 flags to use
MLX4 or MLX5 NICs, respectively, ). To enable debugging, set `CONFIG_DEBUG=y` before building.
```
make clean && make
pushd ksched
make clean && make
popd
sudo ./scripts/setup_machine.sh
```

5) Install Rust and build a synthetic client-server application.

```
curl https://sh.rustup.rs -sSf | sh -s -- -y --default-toolchain=nightly
```
```
cd apps/synthetic
cargo clean
cargo update
cargo build --release
```

6) Run the synthetic application with a client and server. The client
sends requests to the server, which performs a specified amount of
fake work (e.g., computing square roots for 10us), before responding.

On the server:
```
sudo ./iokerneld
./apps/synthetic/target/release/synthetic 192.168.1.3:5000 --config server.config --mode spawner-server
```

On the client:
```
sudo ./iokerneld
./apps/synthetic/target/release/synthetic 192.168.1.3:5000 --config client.config --mode runtime-client
```

## Supported Platforms

This code has been tested most thoroughly on Ubuntu 18.04 with kernel
5.2.0 and Ubuntu 20.04 with kernel 5.4.0.

### NICs
This code has been tested with Intel 82599ES 10 Gbits/s NICs,
Mellanox ConnectX-3 Pro 10 Gbits/s NICs, and Mellanox Connect X-5 40 Gbits/s NICs.
If you use Mellanox NICs, you should install the Mellanox OFED as described in [DPDK's
documentation](https://doc.dpdk.org/guides/nics/mlx4.html). If you use
Intel NICs, you should insert the IGB UIO module and bind your NIC
interface to it (e.g., using the script `./dpdk/usertools/dpdk-setup.sh`).

To enable Jumbo Frames for higher throughput, first enable them in Linux on the
relevant interface like so:
```
ip link set eth0 mtu 9000
```
Then use the (`host_mtu`) option in the config file of each runtime to set the
MTU to the value you'd like, up to the size of the MTU set for the interface.

#### Directpath
Directpath allows runtime cores to directly send packets to/receive packets from the NIC, enabling
higher throughput than when the IOKernel handles all packets.
Directpath is currently only supported with Mellanox ConnectX-5 using Mellanox OFED v4.6 or newer.
NIC firmware must include support for User Context Objects (DEVX) and Software Managed Steering Tables.
For the ConnectX-5, the firmware version must be at least 16.26.1040. Additionally, directpath requires
Linux kernel version 5.0.0 or newer.

To enable directpath, set `CONFIG_DIRECTPATH=y` in build/config before building and add `enable_directpath`
to the config file for all runtimes that should use directpath. Each runtime launched with directpath must
currently run as root and have a unique IP address.

### Storage
This code has been tested with an Intel Optane SSD 900P Series NVMe device.
If your device has op latencies that are greater than 10us, consider updating the device_latency_us
variable (or the known_devices list) in runtime/storage.c.

## More Examples

#### Running a simple block storage server
Ensure that you have compiled Caladan with storage support by setting the appropriate flag in build/config,
and that you have built the synthetic client application.

Compile the C++ bindings and the storage server:
```
make -C bindings/cc
make -C apps/storage_service
```

On the server:
```
sudo ./iokerneld
sudo spdk/scripts/setup.sh
sudo apps/storage_service/storage_server storage_server.config
```

On the client:
```
sudo ./iokerneld
sudo apps/synthetic/target/release/synthetic --config=storage_client.config --mode=runtime-client --mpps=0.55 --protocol=reflex --runtime=10 --samples=10 --threads=20 --transport=tcp 192.168.1.3:5000
```

#### Running with interference

Ensure that you have built the synthetic application on client and server.

Compile the C++ bindings and the memory/cache antagonist:
```
make -C bindings/cc
make -C apps/netbench
```

On the server, run the IOKernel with the interference-aware scheduler (ias),
the synthetic application, and the cache antagonist:
```
sudo ./iokerneld ias
./apps/synthetic/target/release/synthetic 192.168.1.8:5000 --config victim.config --mode spawner-server
./apps/netbench/stress antagonist.config 20 10 cacheantagonist:4090880
```

On the client:
```
sudo ./iokerneld
./apps/synthetic/target/release/synthetic 192.168.1.8:5000 --config client.config --mode runtime-client
```

You should observe that you can stop and start the antagonist and that the
synthetic application's latency is not impacted. In contrast, if you use
Shenango's default scheduler (`sudo ./iokerneld`) on the server, when you run
the antagonist with the synthetic application, the synthetic application's
latency degrades.
