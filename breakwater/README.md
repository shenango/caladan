# Breakwater

Breakwater is a server overload control system for
microseconds-level RPCs. This repo includes 
Shenango implementation of Breakwater as a library of
RPC layer.

## Reproducing results in the paper
The results in the paper were generated with 11 machines
(10 clients and 1 server) of
xl170 in [Cloudlab](https://cloudlab.us) with Ubuntu 18.04 LTS
(64 bit) where all optional configurations below are applied.

1) Configure and build Shenango. (refer to Shenango documentation)
- [xl170] Since xl170 has Mellanox ConnectX-4 NIC, install Mellanox
OFED before building Shenango and set `CONFIG_MLX5=y` in shared.mk
- (optional) Build Shenango with Directpath.
Set `CONFIG_DIRECTPATH=y` in shared.mk
- [xl170] Correct DPDK port for XL170 machine is 1. Change line 233
of iokernel/dpdk.c to `dpdk.port = 1`

2) (optional) Keep CPU in a C-State
```
$ pushd scripts
scripts$ sudo killall cstate
scripts$ gcc cstate.c -o cstate
scripts$ ./cstate 0 &
scripts$ popd
```

3) (optional) Disable frequency scaling
```
$ echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

4) (optional) Disable turbo boost
```
$ echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
```

5) Build C++ bindings for runtime library
```
$ make -C bindings/cc
```

6) Build breakwater library
```
$ cd breakwater
breakwater$ make clean && make
breakwater$ make -C bindings/cc
```

7) Build benchmark application
```
breakwater$ cd apps/netbench
breakwater/apps/netbench$ make clean && make
```

8) Run Shenango IOKernel
```
$ sudo ./iokerneld
```

9) Launch benchmark application (Server)
```
breakwater/apps/netbench$ sudo ./netbench server.config server
```

10) Launch benchmark application (Master Client)
When launching multiple client applications, one of them should be a master client,
and others are agents.
```
breakwater/apps/netbench$ sudo ./netbench client.config client [# threads] [server_ip] [service_time_us] [# agents] [offered_load]
```

11) Launch benchmark application (Agent)
```
breakwater/apps/netbench$ sudo ./netbench client.config agent [master_client_ip] [offered_load]
```
