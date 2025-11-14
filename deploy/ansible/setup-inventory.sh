#!/bin/bash
# Setup script for managing sensitive inventory configuration

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INVENTORY_FILE="$SCRIPT_DIR/inventory.yml"
EXAMPLE_FILE="$SCRIPT_DIR/inventory.yml.example"
VAULT_FILE="$SCRIPT_DIR/inventory.vault"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "=========================================="
echo " Unicity Inventory Setup"
echo "=========================================="
echo

# Check if inventory already exists
if [ -f "$INVENTORY_FILE" ]; then
    echo -e "${YELLOW}Warning: inventory.yml already exists${NC}"
    read -p "Do you want to recreate it? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Keeping existing inventory.yml"
        exit 0
    fi
fi

echo "Choose setup method:"
echo "1) Create from template (recommended)"
echo "2) Use environment variables"
echo "3) Create encrypted with Ansible Vault"
echo "4) Import from environment"
read -p "Choice (1-4): " choice

case $choice in
    1)
        echo -e "\n${GREEN}Creating inventory from template...${NC}"
        cp "$EXAMPLE_FILE" "$INVENTORY_FILE"
        echo -e "${YELLOW}Please edit $INVENTORY_FILE with your server details${NC}"
        echo "Then run: ansible-playbook -i inventory.yml deploy-simple.yml"
        ;;

    2)
        echo -e "\n${GREEN}Creating inventory with environment variables...${NC}"
        cat > "$INVENTORY_FILE" << 'EOF'
# Dynamic inventory using environment variables
all:
  children:
    unicity_nodes:
      hosts:
EOF

        # Generate nodes from environment
        for i in {1..6}; do
            NODE_IP_VAR="NODE${i}_IP"
            NODE_IP="${!NODE_IP_VAR}"
            if [ -n "$NODE_IP" ]; then
                cat >> "$INVENTORY_FILE" << EOF
        node$i:
          ansible_host: $NODE_IP
          node_id: $((i-1))
          p2p_port: 29333
          rpc_port: 29334
EOF
            fi
        done

        cat >> "$INVENTORY_FILE" << 'EOF'
      vars:
        ansible_user: ${ANSIBLE_USER:-root}
        ansible_ssh_private_key_file: ${SSH_KEY:-~/.ssh/id_rsa}
        ansible_python_interpreter: /usr/bin/python3
        unicity_network: ${NETWORK:-regtest}
        docker_data_dir: /var/lib/docker/unicity
        container_name: "unicity-{{ unicity_network }}"
EOF
        echo -e "${GREEN}Created inventory from environment${NC}"
        echo "Set NODE1_IP, NODE2_IP, etc. before running ansible"
        ;;

    3)
        echo -e "\n${GREEN}Creating encrypted inventory with Ansible Vault...${NC}"
        cp "$EXAMPLE_FILE" "$INVENTORY_FILE"
        ansible-vault encrypt "$INVENTORY_FILE"
        echo -e "${GREEN}Inventory encrypted!${NC}"
        echo "Run with: ansible-playbook -i inventory.yml deploy-simple.yml --ask-vault-pass"
        ;;

    4)
        echo -e "\n${GREEN}Import from .env file...${NC}"
        if [ -f "$SCRIPT_DIR/.env" ]; then
            source "$SCRIPT_DIR/.env"
            echo "Loaded configuration from .env"
        else
            echo -e "${RED}No .env file found${NC}"
            echo "Create .env with:"
            echo "  NODE1_IP=x.x.x.x"
            echo "  NODE2_IP=x.x.x.x"
            echo "  ..."
            exit 1
        fi
        # Then create inventory from loaded vars
        $0 2  # Recursively call option 2
        ;;

    *)
        echo -e "${RED}Invalid choice${NC}"
        exit 1
        ;;
esac

echo -e "\n${GREEN}Setup complete!${NC}"