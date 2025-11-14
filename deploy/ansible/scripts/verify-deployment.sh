#!/bin/bash
# Unicity Deployment Verification Script
# Checks that all nodes are running, connected, and synchronized

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
SSH_KEY="${SSH_KEY:-~/.ssh/mykey}"
CONTAINER_NAME="${CONTAINER_NAME:-unicity-regtest}"
MIN_PEERS="${MIN_PEERS:-5}"
SYNC_TOLERANCE="${SYNC_TOLERANCE:-2}" # Allow 2 block difference

# Node list (should match inventory)
declare -a NODES=(
    "178.18.251.16"  # ct20
    "185.225.233.49" # ct21
    "207.244.248.15" # ct22
    "194.140.197.98" # ct23
    "173.212.251.205" # ct24
    "144.126.138.46" # ct25
)

# Test results
declare -a TEST_RESULTS=()
TESTS_PASSED=0
TESTS_FAILED=0

# Helper functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

run_remote() {
    local node=$1
    shift
    ssh -i "$SSH_KEY" -o ConnectTimeout=5 -o StrictHostKeyChecking=no root@"$node" "$@" 2>/dev/null
}

# Check if container is running on a node
check_container_running() {
    local node=$1
    local status=$(run_remote "$node" "docker ps --filter name=$CONTAINER_NAME --format '{{.Status}}' | head -1")

    if [[ "$status" == *"Up"* ]]; then
        return 0
    else
        return 1
    fi
}

# Get peer count for a node
get_peer_count() {
    local node=$1
    run_remote "$node" "docker exec $CONTAINER_NAME unicity-cli getconnectioncount 2>&1" || echo "0"
}

# Get block height for a node
get_block_height() {
    local node=$1
    run_remote "$node" "docker exec $CONTAINER_NAME unicity-cli getblockcount 2>&1" || echo "-1"
}

# Get best block hash for a node
get_best_block() {
    local node=$1
    run_remote "$node" "docker exec $CONTAINER_NAME unicity-cli getbestblockhash 2>&1" || echo "error"
}

# Main verification logic
main() {
    echo "=========================================="
    echo " Unicity Deployment Verification"
    echo "=========================================="
    echo
    echo "Checking ${#NODES[@]} nodes..."
    echo

    # Test 1: Container Status
    echo "1. Container Status Check"
    echo "--------------------------"
    for i in "${!NODES[@]}"; do
        node="${NODES[$i]}"
        echo -n "  Node ct2$i ($node): "

        if check_container_running "$node"; then
            echo -e "${GREEN}✓ Running${NC}"
            ((TESTS_PASSED++))
        else
            echo -e "${RED}✗ Not Running${NC}"
            ((TESTS_FAILED++))
        fi
    done
    echo

    # Test 2: Peer Connectivity
    echo "2. Peer Connectivity Check"
    echo "---------------------------"
    for i in "${!NODES[@]}"; do
        node="${NODES[$i]}"
        peer_count=$(get_peer_count "$node")
        echo -n "  Node ct2$i: $peer_count peers - "

        if [[ "$peer_count" -ge "$MIN_PEERS" ]]; then
            echo -e "${GREEN}✓ OK${NC}"
            ((TESTS_PASSED++))
        else
            echo -e "${RED}✗ Insufficient peers (min: $MIN_PEERS)${NC}"
            ((TESTS_FAILED++))
        fi
    done
    echo

    # Test 3: Blockchain Sync
    echo "3. Blockchain Synchronization Check"
    echo "------------------------------------"
    declare -a heights=()
    declare -a hashes=()

    # Collect block heights and hashes
    for i in "${!NODES[@]}"; do
        node="${NODES[$i]}"
        height=$(get_block_height "$node")
        hash=$(get_best_block "$node" | cut -c1-16)
        heights+=("$height")
        hashes+=("$hash")
        echo "  Node ct2$i: Block $height - Hash ${hash}..."
    done

    # Check if all nodes are synced (within tolerance)
    min_height=$(printf '%s\n' "${heights[@]}" | sort -n | head -1)
    max_height=$(printf '%s\n' "${heights[@]}" | sort -n | tail -1)
    height_diff=$((max_height - min_height))

    echo
    echo -n "  Height difference: $height_diff blocks - "
    if [[ "$height_diff" -le "$SYNC_TOLERANCE" ]]; then
        echo -e "${GREEN}✓ Synchronized${NC}"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}✗ Not synchronized${NC}"
        ((TESTS_FAILED++))
    fi

    # Check if nodes at same height have same hash
    unique_hashes=$(printf '%s\n' "${hashes[@]}" | sort -u | wc -l)
    echo -n "  Block consensus: $unique_hashes unique hashes - "
    if [[ "$unique_hashes" -le "$((SYNC_TOLERANCE + 1))" ]]; then
        echo -e "${GREEN}✓ Consensus achieved${NC}"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}✗ No consensus${NC}"
        ((TESTS_FAILED++))
    fi
    echo

    # Test 4: RPC Responsiveness
    echo "4. RPC Interface Check"
    echo "----------------------"
    for i in "${!NODES[@]}"; do
        node="${NODES[$i]}"
        echo -n "  Node ct2$i: "

        if run_remote "$node" "docker exec $CONTAINER_NAME unicity-cli getinfo &>/dev/null"; then
            echo -e "${GREEN}✓ RPC responsive${NC}"
            ((TESTS_PASSED++))
        else
            echo -e "${RED}✗ RPC not responding${NC}"
            ((TESTS_FAILED++))
        fi
    done
    echo

    # Summary
    echo "=========================================="
    echo " Summary"
    echo "=========================================="
    total_tests=$((TESTS_PASSED + TESTS_FAILED))
    echo "  Tests Passed: $TESTS_PASSED / $total_tests"
    echo "  Tests Failed: $TESTS_FAILED / $total_tests"
    echo

    if [[ "$TESTS_FAILED" -eq 0 ]]; then
        echo -e "${GREEN}✓ All verification checks passed!${NC}"
        echo "  The Unicity network is fully operational."
        exit 0
    else
        echo -e "${RED}✗ Some verification checks failed.${NC}"
        echo "  Please check the failed nodes and retry deployment if necessary."
        exit 1
    fi
}

# Run main function
main "$@"