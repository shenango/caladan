###
### config_remote.py - configuration for remote servers
###

# Public domain or IP of server
SERVER = "server.breakwater.com"
# Public domain or IP of client
CLIENT = "client.breakwater.com"
# Public domain or IP of agents
AGENTS = ["agent1.breakwater.com", "agent2.breakwater.com"]

# Username and SSH credential location to access
# the server, client, and agents via public IP
USERNAME = "breakwater"
KEY_LOCATION = "~/xxx.pem"

# Relative Shenango path from home directory
SHENANGO_PATH = "shenango"

# Network RTT between client and server (in us)
NET_RTT = 10
