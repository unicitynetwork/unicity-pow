# Unicity Docker Commands Reference

Quick reference for managing deployed Unicity nodes.

## Server Access

```bash
# SSH into servers (using mykey)
ssh -i ~/.ssh/mykey root@178.18.251.16  # ct20
ssh -i ~/.ssh/mykey root@185.225.233.49 # ct21
ssh -i ~/.ssh/mykey root@207.244.248.15 # ct22
ssh -i ~/.ssh/mykey root@194.140.197.98 # ct23
ssh -i ~/.ssh/mykey root@173.212.251.205 # ct24
ssh -i ~/.ssh/mykey root@144.126.138.46 # ct25
```

## Container Management

### Check Container Status

```bash
# List running containers
docker ps

# List all containers (including stopped)
docker ps -a

# Filter for unicity containers
docker ps -a | grep unicity

# Get detailed container info
docker inspect unicity-regtest
```

### Container Lifecycle

```bash
# Start container
docker start unicity-regtest

# Stop container
docker stop unicity-regtest

# Restart container
docker restart unicity-regtest

# Remove container
docker rm unicity-regtest

# Remove container forcefully
docker rm -f unicity-regtest
```

### View Logs

```bash
# View all logs
docker logs unicity-regtest

# Follow logs (live tail)
docker logs -f unicity-regtest

# Last 50 lines
docker logs unicity-regtest --tail 50

# Last 100 lines with timestamps
docker logs unicity-regtest --tail 100 --timestamps

# View debug.log inside container
docker exec unicity-regtest tail -f /home/unicity/.unicity/debug.log

# Last 100 lines of debug.log
docker exec unicity-regtest tail -100 /home/unicity/.unicity/debug.log

# Search logs for specific text
docker logs unicity-regtest 2>&1 | grep -i "error"
docker exec unicity-regtest grep -i "VERSION" /home/unicity/.unicity/debug.log
```

## CLI Commands

### IMPORTANT: Correct CLI Syntax

The CLI **does NOT support `-regtest` flag**. It only accepts:
- `--datadir=<path>` - specify data directory
- `--help` - show help

The CLI connects to the daemon via Unix socket at `<datadir>/node.sock`.

### Basic Node Information

```bash
# Get general node info
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getinfo

# Get blockchain info
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getblockchaininfo

# Get block count
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getblockcount

# Get best block hash
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getbestblockhash

# Get difficulty
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getdifficulty
```

### Block Operations

```bash
# Get block hash at specific height
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getblockhash <height>

# Example: Get block hash at height 1
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getblockhash 1

# Get block header by hash
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getblockheader <hash>
```

### Mining

```bash
# Mine 1 block
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity generate 1

# Mine 10 blocks
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity generate 10

# Get mining info
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getmininginfo

# Get network hashrate
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getnetworkhashps
```

### Network & Peers

```bash
# Get peer information
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getpeerinfo

# Add peer connection
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity addnode <ip>:<port> add

# Example: Connect to ct20 from another node
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity addnode 178.18.251.16:29333 add

# Remove peer connection
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity addnode <ip>:<port> remove

# Get network info (if implemented)
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getnetworkinfo
```

### Node Control

```bash
# Stop the node
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity stop

# Get help
docker exec unicity-regtest unicity-cli --help
```

## Ansible Remote Execution

### Single Command Execution

```bash
# Execute command on one server
ansible ct20 -i inventory.yml -m shell -a "docker ps"

# Execute on all servers
ansible unicity_nodes -i inventory.yml -m shell -a "docker ps -a | grep unicity"

# Get info from all nodes
ansible unicity_nodes -i inventory.yml -m shell -a "docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getinfo"

# Check peer count on all nodes
ansible unicity_nodes -i inventory.yml -m shell -a "docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getpeerinfo | grep addr"
```

### Deployment Commands

```bash
# Deploy to specific server
ansible-playbook -i inventory.yml deploy-simple.yml --limit ct20

# Deploy to multiple servers
ansible-playbook -i inventory.yml deploy-simple.yml --limit ct20,ct21,ct22

# Deploy to all servers
ansible-playbook -i inventory.yml deploy-simple.yml

# Deploy only (skip build)
ansible-playbook -i inventory.yml deploy-simple.yml --tags deploy

# Build only (skip deploy)
ansible-playbook -i inventory.yml deploy-simple.yml --tags build

# Build and deploy
ansible-playbook -i inventory.yml deploy-simple.yml --tags build,deploy
```

## Image Management

```bash
# List images
docker images

# Remove image
docker rmi unicity:latest

# Remove image forcefully
docker rmi -f unicity:latest

# Build image manually
docker build -t unicity:latest /path/to/unicity-docker

# Prune unused images
docker image prune

# Prune all unused Docker resources
docker system prune -a
```

## Data Directory Management

```bash
# View data directory contents
docker exec unicity-regtest ls -la /home/unicity/.unicity

# Check data directory size
docker exec unicity-regtest du -sh /home/unicity/.unicity

# View specific files
docker exec unicity-regtest cat /home/unicity/.unicity/peers.json
docker exec unicity-regtest cat /home/unicity/.unicity/banlist.json

# Check permissions
docker exec unicity-regtest ls -la /home/unicity/.unicity/debug.log
```

## Debugging

### Check Container Health

```bash
# View container stats
docker stats unicity-regtest

# Check container resource usage
docker exec unicity-regtest ps aux

# Check running processes
docker top unicity-regtest

# Inspect container configuration
docker inspect unicity-regtest | grep -A10 "Env"
docker inspect unicity-regtest | grep -A10 "Mounts"
```

### Network Debugging

```bash
# Check if port is listening
docker exec unicity-regtest netstat -tlnp | grep 29333

# Test connectivity from container
docker exec unicity-regtest ping -c 3 178.18.251.16

# Check firewall rules (on host)
ufw status

# Check if container can reach peer
docker exec unicity-regtest nc -zv 178.18.251.16 29333
```

### Log Analysis

```bash
# Search for errors
docker logs unicity-regtest 2>&1 | grep -i error

# Search for peer connections
docker exec unicity-regtest grep -i "peer" /home/unicity/.unicity/debug.log

# Search for VERSION/VERACK messages
docker exec unicity-regtest grep -E "VERSION|VERACK" /home/unicity/.unicity/debug.log

# Count peer connections over time
docker exec unicity-regtest grep "Added peer" /home/unicity/.unicity/debug.log | wc -l

# Find handshake issues
docker exec unicity-regtest grep -i "timeout\|disconnect" /home/unicity/.unicity/debug.log
```

## Multi-Node Operations

### Connect Nodes to Each Other

```bash
# From ct21, connect to ct20
ssh -i ~/.ssh/mykey root@185.225.233.49 "docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity addnode 178.18.251.16:29333 add"

# From ct22, connect to ct20 and ct21
ssh -i ~/.ssh/mykey root@207.244.248.15 "docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity addnode 178.18.251.16:29333 add"
ssh -i ~/.ssh/mykey root@207.244.248.15 "docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity addnode 185.225.233.49:29333 add"
```

### Check Sync Status Across Nodes

```bash
# Get block height from all nodes
for ip in 178.18.251.16 185.225.233.49 207.244.248.15 194.140.197.98 173.212.251.205 144.126.138.46; do
  echo "=== $ip ==="
  ssh -i ~/.ssh/mykey root@$ip "docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getinfo | grep blocks"
done

# Check peer count on all nodes
for ip in 178.18.251.16 185.225.233.49 207.244.248.15; do
  echo "=== $ip ==="
  ssh -i ~/.ssh/mykey root@$ip "docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getpeerinfo | grep -c addr || echo 0"
done
```

### Mine on Different Nodes

```bash
# Mine 5 blocks on ct20
ssh -i ~/.ssh/mykey root@178.18.251.16 "docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity generate 5"

# Mine 3 blocks on ct21
ssh -i ~/.ssh/mykey root@185.225.233.49 "docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity generate 3"
```

## Common Workflows

### Complete Node Restart

```bash
# Stop container
docker stop unicity-regtest

# Remove container
docker rm unicity-regtest

# Start fresh container
docker run -d \
  --name unicity-regtest \
  --restart unless-stopped \
  -p 29333:29333 \
  -p 29334:29334 \
  -v /var/lib/unicity:/home/unicity/.unicity \
  -e UNICITY_NETWORK=regtest \
  -e UNICITY_MAXCONNECTIONS=10 \
  unicity:latest
```

### Reset Node Data

```bash
# Stop container
docker stop unicity-regtest

# Remove container
docker rm unicity-regtest

# Clear blockchain data (CAUTION!)
rm -rf /var/lib/unicity/*

# Fix permissions
chown -R 1000:1000 /var/lib/unicity

# Restart container
docker run -d --name unicity-regtest ... (same as above)
```

### Update Code and Redeploy

```bash
# From local machine (in ansible directory)
ansible-playbook -i inventory.yml deploy-simple.yml --limit ct20 --tags build,deploy

# Or manually on server
ssh -i ~/.ssh/mykey root@178.18.251.16
docker stop unicity-regtest
docker rm unicity-regtest
# ... copy new code ...
docker build -t unicity:latest /tmp/unicity-build
docker run -d ... (start container)
```

## Troubleshooting

### Container Won't Start

```bash
# Check logs for errors
docker logs unicity-regtest 2>&1 | tail -50

# Check if permissions are correct
ls -la /var/lib/unicity

# Should be owned by UID 1000
chown -R 1000:1000 /var/lib/unicity

# Check if ports are already in use
netstat -tlnp | grep 29333
```

### No Peer Connections

```bash
# Check if node is listening
docker exec unicity-regtest netstat -tlnp | grep 29333

# Check firewall
ufw status
ufw allow 29333/tcp

# Check peer addresses
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getpeerinfo

# Check debug log for connection attempts
docker exec unicity-regtest grep -i "connect\|peer" /home/unicity/.unicity/debug.log | tail -20
```

### Handshake Timeout

```bash
# Check for VERSION/VERACK in logs
docker exec unicity-regtest grep -E "VERSION|VERACK|timeout" /home/unicity/.unicity/debug.log

# Check network connectivity
docker exec unicity-regtest ping -c 3 <peer_ip>

# Check if firewall is blocking
telnet <peer_ip> 29333
```

### Blocks Not Syncing

```bash
# Check peer info
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getpeerinfo

# Check if peer is at higher height
# On peer node:
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getinfo

# Check for sync errors in logs
docker exec unicity-regtest grep -i "sync\|header\|block" /home/unicity/.unicity/debug.log | tail -30
```

## Quick Copy-Paste Commands

### Full Node Status Check

```bash
echo "=== Container Status ===" && \
docker ps -a | grep unicity && \
echo "=== Node Info ===" && \
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getinfo && \
echo "=== Peer Info ===" && \
docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity getpeerinfo && \
echo "=== Recent Logs ===" && \
docker logs unicity-regtest --tail 20
```

### Connect to All Other Nodes (run on each node)

```bash
# On ct20 (connect to ct21-ct25)
for ip in 185.225.233.49 207.244.248.15 194.140.197.98 173.212.251.205 144.126.138.46; do
  docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity addnode $ip:29333 add
done

# On ct21 (connect to ct20,ct22-ct25)
for ip in 178.18.251.16 207.244.248.15 194.140.197.98 173.212.251.205 144.126.138.46; do
  docker exec unicity-regtest unicity-cli --datadir=/home/unicity/.unicity addnode $ip:29333 add
done
```

## Environment Variables

The container accepts these environment variables:

```bash
UNICITY_NETWORK     # Network type: mainnet, testnet, regtest (default: mainnet)
UNICITY_PORT        # P2P port (default: network-specific)
UNICITY_LISTEN      # Enable listening: 0 or 1 (default: 1)
UNICITY_SERVER      # Not used (RPC always enabled)
UNICITY_VERBOSE     # Verbose logging: 0 or 1 (default: 0)
UNICITY_MAXCONNECTIONS  # Max peer connections (default: 10)
```

## Notes

- **Container name**: `unicity-regtest` (for regtest network)
- **Data directory (host)**: `/var/lib/unicity`
- **Data directory (container)**: `/home/unicity/.unicity`
- **Container user**: `unicity` (UID 1000)
- **P2P ports**: 9590 (mainnet), 19333 (testnet), 29333 (regtest)
- **RPC socket**: `<datadir>/node.sock` (Unix socket, not TCP)
- **Debug log**: `<datadir>/debug.log`
