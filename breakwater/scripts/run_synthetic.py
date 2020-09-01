import paramiko
import os
from time import sleep
from util import *
from config_remote import *

### Experiemnt Configuration ###
# Server overload algorithm (breakwater, seda, dagor)
OVERLOAD_ALG = "breakwater"

# The number of client connections
NUM_CONNS = 1000

# Average service time (in us)
ST_AVG = 10

# Service time distribution
#    exp: exponential
#    const: constant
#    bimod: bimodal
ST_DIST = "exp"

# List of offered load
OFFERED_LOADS = [100000, 200000, 300000, 400000, 500000, 600000, 700000, 800000, 900000,
                 1000000, 1100000, 1200000, 1400000, 1600000]

ENABLE_DIRECTPATH = True

NUM_CORES_SERVER = 10
NUM_CORES_CLIENT = 16
### End of configuration ###

# SLO = 10 * (average RPC processing time + network RTT)
slo = (ST_AVG + NET_RTT) * 10

### Function definitions ###
def generate_shenango_config(is_server ,conn, ip, netmask, gateway, num_cores, directpath):
    config_name = ""
    config_string = ""
    if is_server:
        config_name = "server.config"
        config_string = "host_addr {}".format(ip)\
                      + "\nhost_netmask {}".format(netmask)\
                      + "\nhost_gateway {}".format(gateway)\
                      + "\nruntime_kthreads {:d}".format(num_cores)
    else:
        config_name = "client.config"
        config_string = "host_addr {}".format(ip)\
                      + "\nhost_netmask {}".format(netmask)\
                      + "\nhost_gateway {}".format(gateway)\
                      + "\nruntime_kthreads {:d}".format(num_cores)\
                      + "\nruntime_spinning_kthreads {:d}".format(num_cores)

    if directpath:
        config_string += "\nenable_directpath 1"

    cmd = "cd {}/breakwater && echo \"{}\" > {} "\
            .format(SHENANGO_PATH,config_string, config_name)

    return execute_command([conn], cmd, True)
### End of function definition ###

NUM_AGENT = len(AGENTS)

# configure Shenango IPs for config
server_ip = "192.168.1.200"
client_ip = "192.168.1.100"
agent_ips = []
netmask = "255.255.255.0"
gateway = "192.168.1.1"

for i in range(NUM_AGENT):
    agent_ip = "192.168.1." + str(101 + i);
    agent_ips.append(agent_ip)

k = paramiko.RSAKey.from_private_key_file(KEY_LOCATION)
# connection to server
server_conn = paramiko.SSHClient()
server_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
server_conn.connect(hostname = SERVER, username = USERNAME, pkey = k)

# connection to client
client_conn = paramiko.SSHClient()
client_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client_conn.connect(hostname = CLIENT, username = USERNAME, pkey = k)

# connections to agents
agent_conns = []
for agent in AGENTS:
    agent_conn = paramiko.SSHClient()
    agent_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    agent_conn.connect(hostname = agent, username = USERNAME, pkey = k)
    agent_conns.append(agent_conn)

# Clean-up environment
print("Cleaning up machines...")
cmd = "sudo killall -9 netbench & sudo killall -9 iokerneld"
execute_command([server_conn, client_conn] + agent_conns,
                cmd, True)
sleep(1)

# Remove temporary output
cmd = "cd {} && rm output.csv output.json".format(SHENANGO_PATH)
execute_command([client_conn], cmd, True)

# Generating config files
print("Generating config files...")
generate_shenango_config(True, server_conn, server_ip, netmask, gateway,
                         NUM_CORES_SERVER, ENABLE_DIRECTPATH)
generate_shenango_config(False, client_conn, client_ip, netmask, gateway,
                         NUM_CORES_CLIENT, ENABLE_DIRECTPATH)
for i in range(NUM_AGENT):
    generate_shenango_config(False, agent_conns[i], agent_ips[i], netmask,
                             gateway, NUM_CORES_CLIENT, ENABLE_DIRECTPATH)

# Rebuild Shanango
print("Building Shenango...")
cmd = "cd {} && make clean && make && make -C bindings/cc".format(SHENANGO_PATH)
execute_command([server_conn, client_conn] + agent_conns,
                cmd, True)

# Build Breakwater
print("Building Breakwater...")
cmd = "cd {}/breakwater && make clean && make && make -C bindings/cc".format(SHENANGO_PATH)
execute_command([server_conn, client_conn] + agent_conns,
                 cmd, True)

# Build Netbench
print("Building netbench...")
cmd = "cd {}/breakwater/apps/netbench && make clean && make".format(SHENANGO_PATH)
execute_command([server_conn, client_conn] + agent_conns,
                cmd, True)

# Execute IOKernel
iok_sessions = []
print("Executing IOKernel...")
cmd = "cd {} && sudo ./iokerneld".format(SHENANGO_PATH)
iok_sessions += execute_command([server_conn, client_conn] + agent_conns,
                               cmd, False)

sleep(1)

for offered_load in OFFERED_LOADS:
    print("Load = {:d}".format(offered_load))
    # Execute netbench application
    # - server
    print("\tExecuting server...")
    cmd = "cd {}/breakwater && sudo ./apps/netbench/netbench {} server.config server"\
            .format(SHENANGO_PATH, OVERLOAD_ALG)
    server_session = execute_command([server_conn], cmd, False)
    server_session = server_session[0]
    
    sleep(1)

    # - client
    print("\tExecuting client...")
    client_agent_sessions = []
    cmd = "cd {}/breakwater && sudo ./apps/netbench/netbench {} client.config client"\
            " {:d} {} {:d} {} {:d} {:d} {:d}"\
            .format(SHENANGO_PATH, OVERLOAD_ALG, NUM_CONNS, server_ip, ST_AVG, ST_DIST,
                    slo ,NUM_AGENT, offered_load)
    client_agent_sessions += execute_command([client_conn], cmd, False)

    sleep(1)
    
    # - agent
    print("\tExecuting agents...")
    cmd = "cd {}/breakwater && sudo ./apps/netbench/netbench {} client.config agent"\
            " {}".format(SHENANGO_PATH, OVERLOAD_ALG, client_ip)
    client_agent_sessions += execute_command(agent_conns, cmd, False)

    # Wait for client and agents
    print("\tWaiting for client and agents...")
    for client_agent_session in client_agent_sessions:
        client_agent_session.recv_exit_status()

    # Kill server
    cmd = "sudo killall -9 netbench"
    execute_command([server_conn], cmd, True)

    # Wait for server to be killed
    server_session.recv_exit_status()

    sleep(1)

# Kill IOKernel
cmd = "sudo killall -9 iokerneld"
execute_command([server_conn, client_conn] + agent_conns, cmd, True)

# Wait for IOKernel sessions
for iok_session in iok_sessions:
    iok_session.recv_exit_status()

# Close connections
server_conn.close()
client_conn.close()
for agent_conn in agent_conns:
    agent_conn.close()

# Create output directory
if not os.path.exists("outputs"):
    os.mkdir("outputs")

# Move output.csv and output.json
print("Collecting outputs...")
cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no {}@{}:~/{}/breakwater/output.csv ./"\
        .format(KEY_LOCATION, USERNAME, CLIENT, SHENANGO_PATH)
os.system(cmd)

output_prefix = "{}_{}_{:d}_nconn_{:d}".format(OVERLOAD_ALG, ST_DIST, ST_AVG, NUM_CONNS)
# Print Headers
header = "num_clients,offered_load,throughput,goodput,cpu,min,mean,p50,p90,p99,p999,p9999"\
        ",max, p1_win,mean_win,p99_win,p1_q,mean_q,p99_q,mean_stime,p99_stime,server:rx_pps"\
        ",server:tx_pps,server:rx_bps,server:tx_bps,server:rx_drops_pps, server:rx_ooo_pps"\
        ",server:winu_rx_pps,server:winu_tx_pps,server:win_tx_wps,server:req_rx_pps"\
        ",server:req_drop_rate,server:resp_tx_pps,client:min_tput,client:max_tput"\
        ",client:winu_rx_pps,client:resp_rx_pps,client:req_tx_pps"\
        ",client:win_expired_wps,client:req_dropped_rps"
cmd = "echo \"{}\" > outputs/{}.csv".format(header, output_prefix)
os.system(cmd)

cmd = "cat output.csv >> outputs/{}.csv".format(output_prefix)
os.system(cmd)

# Remove temp outputs
cmd = "rm output.csv"
os.system(cmd)

print("Done.")
