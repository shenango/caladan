###
### config_remote.py - configuration for remote servers
###

# Public domain or IP of server
SERVER = "hp128.utah.cloudlab.us"
# Public domain or IP of client
CLIENT = "hp131.utah.cloudlab.us"
# Public domain or IP of agents
AGENTS = ["hp130.utah.cloudlab.us", "hp160.utah.cloudlab.us", "hp145.utah.cloudlab.us"]

# Username and SSH credential location to access
# the server, client, and agents via public IP
USERNAME = "inhocho"
KEY_LOCATION = "/users/inhocho/inhocho.pem"

# Relative Shenango path from home directory
SHENANGO_PATH = "shenango"

# Network RTT between client and server (in us)
NET_RTT = 10
