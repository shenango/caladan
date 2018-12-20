# Shenango

Shenango is a system that enables servers in datacenters to
simultaneously provide low tail latency and high CPU efficiency, by
rapidly reallocating cores across applications, at timescales as small
as every 5 microseconds.

## How to Run Shenango

1) Clone the Shenango repository.

```
git clone https://github.com/abelay/shenango
cd shenango
```

2) Setup DPDK/SPDK and build the IOKernel and Shenango runtime.

```
./dpdk.sh
./spdk.sh
./scripts/setup_machine.sh
make clean && make
```

To enable debugging, build with `make DEBUG=1`.

3) Install Rust and build a synthetic client-server application.

```
curl https://sh.rustup.rs -sSf | sh
rustup default nightly
```
```
cd apps/synthetic
cargo clean
cargo update
cargo build --release
```

4) Run the synthetic application with a client and server. The client
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

This code has been tested most thoroughly on Ubuntu 18.04, with kernel
4.15.0. It has been tested with Intel 82599ES 10 Gbits/s NICs and
Mellanox ConnectX-3 Pro 10 Gbits/s NICs. If you use Mellanox NICs, you
should install the Mellanox OFED as described in [DPDK's
documentation](https://doc.dpdk.org/guides/nics/mlx4.html). If you use
Intel NICs, you should insert the IGB UIO module and bind your NIC
interface to it (e.g., using the script `./dpdk/usertools/dpdk-setup.sh`).
