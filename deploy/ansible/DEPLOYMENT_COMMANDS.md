# Unicity Deployment Commands

## Quick Reference

### Deploy Testnet
```bash
cd /Users/mike/Code/unicity-full/deploy/ansible
ansible-playbook -i inventory.yml deploy-simple.yml -e "target_network_group=unicity_testnet"
```

### Deploy Mainnet
```bash
cd /Users/mike/Code/unicity-full/deploy/ansible
ansible-playbook -i inventory.yml deploy-simple.yml -e "target_network_group=unicity_mainnet"
```

### Deploy Regtest
```bash
cd /Users/mike/Code/unicity-full/deploy/ansible
ansible-playbook -i inventory.yml deploy-simple.yml -e "target_network_group=unicity_regtest"
```

## Mining

### Start Mining on Testnet
```bash
cd /Users/mike/Code/unicity-full/deploy/ansible
ansible-playbook -i inventory.yml start-mining.yml -e "target_network_group=unicity_testnet"
```

### Start Mining on Mainnet
```bash
cd /Users/mike/Code/unicity-full/deploy/ansible
ansible-playbook -i inventory.yml start-mining.yml -e "target_network_group=unicity_mainnet"
```

### Start Mining on Regtest
```bash
cd /Users/mike/Code/unicity-full/deploy/ansible
ansible-playbook -i inventory.yml start-mining.yml -e "target_network_group=unicity_regtest"
```

## Check Deployment Status

### Check All Nodes (both networks)
```bash
for host in ct20 ct21 ct22 ct23 ct24 ct25; do
  echo "=== $host ==="
  echo -n "Testnet: "
  ssh $host "docker exec unicity-testnet unicity-cli getblockchaininfo 2>/dev/null | grep -E '\"chain\"|\"blocks\"' | head -2 | tr '\n' ' '" || echo "Not running"
  echo ""
  echo -n "Mainnet: "
  ssh $host "docker exec unicity-mainnet unicity-cli getblockchaininfo 2>/dev/null | grep -E '\"chain\"|\"blocks\"' | head -2 | tr '\n' ' '" || echo "Not running"
  echo ""
done
```

### Quick Status Check (first 3 nodes)
```bash
for host in ct20 ct21 ct22; do
  echo "=== $host ==="
  echo -n "Testnet: "
  ssh $host "docker exec unicity-testnet unicity-cli getblockcount 2>/dev/null || echo 'Not running'"
  echo -n "Mainnet: "
  ssh $host "docker exec unicity-mainnet unicity-cli getblockcount 2>/dev/null || echo 'Not running'"
done
```

## Clean Up / Reset

### Stop and Remove All Containers
```bash
for host in ct20 ct21 ct22 ct23 ct24 ct25; do
  echo "=== Cleaning $host ==="
  ssh $host "docker stop \$(docker ps -aq --filter 'name=unicity') 2>/dev/null && docker rm \$(docker ps -aq --filter 'name=unicity') 2>/dev/null || echo 'No containers found'"
done
```

### Wipe Blockchain Data (DANGEROUS - deletes all blocks)
```bash
for host in ct20 ct21 ct22 ct23 ct24 ct25; do
  echo "=== Wiping $host blockchain data ==="
  ssh $host "rm -rf /var/lib/unicity/*"
done
```

## Network Details

### Testnet
- Chain: test
- Port: 19590
- Container: unicity-testnet
- Data Dir: /var/lib/unicity/testnet
- Block Target: 2 minutes
- ASERT Half-life: 1 hour (30 blocks)

### Mainnet
- Chain: main
- Port: 9590
- Container: unicity-mainnet
- Data Dir: /var/lib/unicity/mainnet
- Block Target: 1 hour
- ASERT Half-life: 2 days (48 blocks) - **Updated 2025-10-25**

### Regtest
- Chain: regtest
- Port: 29590
- Container: unicity-regtest
- Data Dir: /var/lib/unicity/regtest
- Block Target: 2 minutes (instant mining)

## Important Notes

1. **Playbook Fix (2025-10-25)**: The playbook now uses `set_fact` to properly handle same hosts in multiple groups. Must use `-e "target_network_group=..."` syntax.

2. **ASERT Update (2025-10-25)**: Mainnet ASERT half-life changed from 12 days to 2 days for 6x faster difficulty convergence. Both networks reset from genesis with updated binary.

3. **Inventory Structure**: Same physical hosts (ct20-ct25) run both testnet and mainnet containers simultaneously on different ports.

4. **Deployment Log Locations**:
   - Testnet: `/tmp/testnet-deploy.log`
   - Mainnet: `/tmp/mainnet-deploy.log`
