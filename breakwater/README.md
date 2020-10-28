# Breakwater
Breakwater is a server overload control system for
microseconds-level RPCs. This repo includes 
Shenango implementation of Breakwater as a library of
RPC layer between network transport and application.
Breakwater shares the Shenango code-base with
[Caladan](https://github.com/joshuafried/caladan-ae),
but Breakwater is independent to Caladan.

## Supported platform
Breakwater requires an Intel server with many cores equipped
with an Intel NIC based on 82599 chip or Mellanox ConnectX-4
or ConnectX-5 NIC. For the best performance,
server and client machines with Mellanox NICs connected with
a low latency switch are necessary. Breakwater has been tested
on Ubuntu 18.04 with Linux kernel 4.15.0.

## Terminology
- Server: RPC server which is serving RPC request
- Client: A single Shenango process which generates load and
controls agents to syncrhonize load generation. 
- Agent: Shenango processes generating load while controlled by
of client. (One of the load generating processes become client
and others are agents.)
- Observer: A machine which commands to server, client, and agents
and collects data from them. Observer could be the same as server,
client, or agent machines.

## Running Breakwater
0. Install dependencies
```
$ sudo apt-get update
$ sudo apt-get install -y libnuma-dev libaio1 libaio-dev uuid-dev libcunit1 libcunit1-doc libcunit1-dev libmnl-dev cmake python3 python3-pip
$ sudo python3 -m pip install paramiko
```

1. Clone this repository.

2. Build submodules
```
$ make submodules
```

3. Modify `build/config` according to the experiment environment.
For the best performance, set `CONFIG_DIRECTPATH=y`, but please
note that directpath is supported with Mellanox ConnectX-4 and
CoonectX-5 Mellanox NICs.


4. Build Shenango and execute the 
`scripts/setup_machine.sh` script in `breakwater` directory.
```
$ make clean && make
$ make -C bindings/cc
$ cd breakwater
breakwater$ sudo ./scripts/setup_machine.sh
```

5. Build applications.
```
breakwater$ make -C apps/netbench/
```

6. Start IOKernel (hardware timestamp feature is under testing)
```
breakwater$ sudo ./iokerneld no_hw_qdel
```

7. Start application. The following example will start a server with Breakwater and make a client to generate workload with exponential distribution (10us average and 100us of SLO) at a rate of 100k requests/s by 100 threads.

On the server:
```
breakwater$ sudo ./apps/netbench/netbench breakwater ../server.config server
```

On the client:
```
breakwater$ sudo ./apps/netbench/netbench breakwater ../client.config client 100 192.168.1.3 10 exp 100 0 100000
```

## Reproducing paper results
Please refer to [breakwater-artifact](https://github.com/inhocho89/breakwater-artifact) repository for experiment scripts to reproduce the paper results.
