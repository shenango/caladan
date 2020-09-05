#!/usr/bin/env python3

import paramiko
import os
from util import *
from config_remote import *

k = paramiko.RSAKey.from_private_key_file(KEY_LOCATION)
# connection to server
server_conn = paramiko.SSHClient()
server_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
server_conn.connect(hostname = SERVER, username = USERNAME, pkey = k)

# connection to client
client_conn = paramiko.SSHClient()
client_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client_conn.connect(hostname = CLIENT, username = USERNAME, pkey = k)

# connection to agents
agent_conns = []
for agent in AGENTS:
    agent_conn = paramiko.SSHClient()
    agent_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    agent_conn.connect(hostname = agent, username = USERNAME, pkey = k)
    agent_conns.append(agent_conn)

# distributing code-base
print("Distributing sources...")
repo_name = (os.getcwd().split('/'))[-2]
# - server
cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no -r ../../{}"\
        " {}@{}:~/{} > /dev/null"\
        .format(KEY_LOCATION, repo_name, USERNAME, SERVER, SHENANGO_PATH)
execute_local(cmd);
# - client
cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no -r ../../{}"\
        " {}@{}:~/{} > /dev/null"\
        .format(KEY_LOCATION, repo_name, USERNAME, CLIENT, SHENANGO_PATH)
execute_local(cmd)
# - agents
for agent in AGENTS:
    cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no -r ../../{}"\
            " {}@{}:~/{} > /dev/null"\
            .format(KEY_LOCATION, repo_name, USERNAME, agent, SHENANGO_PATH)
    execute_local(cmd)

# install sub-modules
print("Building submodules... (it may take a few mintues)")
cmd = "cd ~/{} && make submodules".format(SHENANGO_PATH)
execute_remote([server_conn, client_conn] + agent_conns, cmd, True)

# patch and build shenango
print("Patching and building Shenango...")
cmd = "cd ~/{}/breakwater && ./build/patch_xl170.sh".format(SHENANGO_PATH)
execute_remote([server_conn, client_conn] + agent_conns, cmd, True)

# settting up machines
print("Setting up machines...")
cmd = "cd ~/{}/breakwater && sudo ./scripts/setup_machine.sh".format(SHENANGO_PATH)
execute_remote([server_conn, client_conn] + agent_conns, cmd, True)

# update RTT of Breakwater
print("Setting up breakwater...")
cmd = "cd ~/{}/breakwater && sed -i \'s/#define SBW_RTT_US\t\t\t10/"\
        "#define SBW_RTT_US\t\t\t{:d}/g\' src/bw_config.h"\
        .format(SHENANGO_PATH, NET_RTT)
execute_remote([server_conn, client_conn] + agent_conns, cmd, True)

print("Building Breakwater...")
cmd = "cd ~/{}/breakwater && make clean && make && make -C bindings/cc"\
        .format(SHENANGO_PATH)
execute_remote([server_conn, client_conn] + agent_conns, cmd, True)

print("Done.")
