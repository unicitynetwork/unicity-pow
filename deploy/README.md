# Unicity Deployment

This directory contains all deployment and operations related files for Unicity.

## Directory Structure

```
deploy/
├── ansible/               # Ansible automation
│   ├── inventory.yml      # Server inventory (6 nodes)
│   ├── deploy-simple.yml  # Main deployment playbook
│   ├── ansible.cfg        # Ansible configuration
│   └── scripts/           # Helper scripts
│       ├── setup-mesh.sh  # Establish peer connections
│       └── verify-deployment.sh  # Health checks
├── docker/                # Docker containerization
│   ├── Dockerfile         # Multi-stage build for node
│   └── docker-entrypoint.sh  # Container entry point
└── scripts/               # General deployment scripts
```

## Quick Start

### Deploy to Remote Nodes

```bash
cd deploy/ansible
ansible-playbook -i inventory.yml deploy-simple.yml
```

This will:
1. Build Docker images on all nodes
2. Start containers with correct configuration
3. Automatically establish peer connections
4. Verify deployment health

### Build Docker Image Locally

```bash
# From project root
docker build -f deploy/docker/Dockerfile -t unicity:latest .
```

### Run Local Node

```bash
docker run -d \
  --name unicity \
  -p 9590:9590 \
  -v ~/.unicity:/home/unicity/.unicity \
  -e UNICITY_NETWORK=mainnet \
  unicity:latest
```

Inbound connections are enabled by default. To disable inbound, set UNICITY_LISTEN=0.

## Network Configuration

- **Mainnet Port**: 9590
- **Testnet Port**: 19590
- **Regtest Port**: 29590 (used for testing)

## Server Inventory

The network consists of 6 nodes:
- ct20: 178.18.251.16
- ct21: 185.225.233.49
- ct22: 207.244.248.15
- ct23: 194.140.197.98
- ct24: 173.212.251.205
- ct25: 144.126.138.46

## Maintenance

### Verify Network Health

```bash
bash deploy/ansible/scripts/verify-deployment.sh
```

### Establish Peer Connections

```bash
bash deploy/ansible/scripts/setup-mesh.sh
```

## Notes

- RPC uses Unix domain sockets (`datadir/node.sock`) for security
- No config file support - uses command-line parameters only
- Automatic peer discovery not implemented for regtest
- NAT/UPnP support disabled in Docker builds