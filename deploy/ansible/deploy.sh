#!/bin/bash
# Quick deployment script for Unicity regtest network

set -e

INVENTORY="inventory.yml"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Unicity Ansible Deployment${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Check prerequisites
echo -e "${YELLOW}Checking prerequisites...${NC}"

if ! command -v ansible &> /dev/null; then
    echo -e "${RED}ERROR: Ansible is not installed${NC}"
    echo "Install with: brew install ansible (macOS) or sudo apt install ansible (Ubuntu)"
    exit 1
fi

if ! ansible-galaxy collection list | grep -q community.docker; then
    echo -e "${YELLOW}Installing community.docker collection...${NC}"
    ansible-galaxy collection install community.docker
fi

# Test SSH connectivity
echo -e "${YELLOW}Testing SSH connectivity to servers...${NC}"
if ! ansible unicity_nodes -i $INVENTORY -m ping &> /dev/null; then
    echo -e "${RED}WARNING: Cannot connect to some servers via SSH${NC}"
    echo "Please ensure SSH access is configured for all servers"
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
else
    echo -e "${GREEN}✓ All servers accessible${NC}"
fi

# Deploy
echo ""
echo -e "${YELLOW}Starting deployment...${NC}"
ansible-playbook -i $INVENTORY deploy-unicity.yml

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Deployment Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "Next steps:"
echo "  1. Check network status: ansible-playbook -i $INVENTORY check-sync.yml"
echo "  2. Mine blocks: ansible-playbook -i $INVENTORY mine-blocks.yml -e 'blocks=10'"
echo "  3. Verify sync: ansible-playbook -i $INVENTORY check-sync.yml --tags summary"
echo ""
