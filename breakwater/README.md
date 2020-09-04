# Breakwater
Breakwater is a server overload control system for
microseconds-level RPCs. This repo includes 
Shenango implementation of Breakwater as a library of
RPC layer between network transport and application.

## Supported platform
Breakwater requires an Intel server with many cores equipped
with an Intel NIC based on 82599 chip or Mellanox ConnectX-4
or ConnectX-5 NIC. For the best performance,
server and client machines with Mellanox NICs connected with
a low latency switch are necessary.

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
1. Clone this repository to each machine.
```
$ git clone [to_be_upated]
```

2. Initialize the submodules
```
shenango$ ./build/init_submodules.sh
```

3. Modify `build/config` according to the experiment environment.
For the best performance, set `CONFIG_DIRECTPATH=y`, but please
note that directpath is supported with Mellanox ConnectX-4 and
CoonectX-5 Mellanox NICs.

4. If the machine is equipped with Mellanox ConnectX-4 NIC,
apply ConnectX-4 patch.
```
shenango$ git apply breakwater/build/connectx-4.patch
```

5. Build Shenango and execute the 
`scripts/setup_machine.sh` script in `breakwater` directory.
```
shenango$ make clean && make
shenango$ make -C bindings/cc
shenango$ cd breakwater
shenango/breakwater$ sudo ./scripts/setup_machine.sh
```

6. At observer machine, provide the SSH information of machines in
`scripts/config_remote.py`, and execute `scripts/run_synthetic.py`
to run experiment with synthetic workload.  Default experiment
scripts is for breakwater with 1,000 clients, 10 us average service
time which is exponentially distributed. Once an experiment
finished, you can see the csv-formatted output in `outputs`
directory. Modify the configuration of experiment script for
different overload control mechanisms, average service time,
service time distribution, and the number of clients.
```
shenango/breakwater$ python3 scripts/run_synthetic.py
```
You can modify overload controls' parameters in
`src/bw_config.h` (for Breakwater), `src/dg_config.h` (for Dagor),
and `src/sd_config.h` (for SEDA). If you modify parameters in
observer machine, `scripts/run_synthetic.py` script will distribute
updated configs to all the other machines.

## Quick Start (with Cloudlab XL170)
If you have access to the Cloudlab, you can simply follow the
instruction below to reproduce the results in the paper.
Please note that Breakwater performance may vary based on the
topologies of the servers. It produces the best performance
when all the servers are connected to a single switch.

1. Launch the Cloudlab experiment with multiple bare-metal
machines. You can see the sample Cloudlab profile [here](
https://www.cloudlab.us/p/CreditRPC/breakwater-xl170)
consisting of twelve machines (1 for observer, 1 for server,
and 10 for clients).
You can create your own Cloudlab profile using our pre-built
Cloudlab disk image (URN:
urn:publicid:IDN+utah.cloudlab.us+image+creditrpc-PG0:breakwater-xl170-2)
which is Ubuntu 18.04 LTS with Mellanox OFED driver and
dependencies installed.
**[For artifact evaluators]** If you
don't have access to the Cloudlab and need one, we can provide
the SSH access to the machines after we launch an experiment.
However, please note that because an experiment expires after 16 hours,
you will need to make another request to us after expiration.
Further, as Cloudlab is shared with other researchers, its
availability is not guaranteed.

2. Once you create an experiment, clone this repository to the observer
machine.
```
$ git clone [to_be_updated]
```

3. At the observer, provide the information on remote servers to
setup Shenango and Breakwater in `scripts/config_remote.py`, and
execute `scripts/setup_remote_xl170.py`.
```
$ cd shenango/breakwater
# Modify config_remote.py
shenango/breakwater$ vim scripts/config_remote.py
shenango/breakwater$ python3 scripts/setup_remote_xl170.py
```

4. You can execute `scripts/run_synthetic.py` at observer to experiment
with synthetic workload. Default experiment scripts is for breakwater
with 1,000 clients, exponentially distributed service time with 10 us
average. Once an experiment finished, you can
see the csv-formatted output in `outputs` directory. Modify the
configuration of experiment script to change overload control
mechanisms, average service time, service time distribution, and
the number of clients.
```
shenango/breakwater$ python3 scripts/run_synthetic.py
```
You can modify overload controls' parameters in
`src/bw_config.h` (for Breakwater), `src/dg_config.h` (for Dagor),
and `src/sd_config.h` (for SEDA). If you modify parameters in
observer machine, `scripts/run_synthetic.py` script will distribute
updated configs to all the other machines.

## Tips for reproducing the paper results
- We provide recommended parameter values in `src/bw_config.h`,
`src/dg_config.h`, and `src/sd_config.h` in Cloudlab xl170 experiment
environment with 1,000 clients. Because Dagor and SEDA is sensitive to
parameter values you may need to try adjust parameter values with experiment
environment.

- As we have shown in Figure 7, SEDA may take a long time to recover
its rate after it overreact to the congestion. To get the stable number for
SEDA, you may consider extending experiment time (which require more memory
space especially for shorter average service time), or repeating experiments
and take median or the best performance for SEDA.

- The results in the paper were produced from set of xl170 machines connected
with a single ToR switch in Cloudlab. If machines are connected with multiple
ToR switches and spine switch, the actual number might vary from the paper, but
you can still observe the benefit of Breakwater.
