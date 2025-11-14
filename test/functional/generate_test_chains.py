#!/usr/bin/env python3
# Generate pre-mined test chains for fork resolution testing

import sys
import time
import tempfile
import shutil
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "test_framework"))
from test_node import TestNode

GREEN = '\033[92m'
RED = '\033[91m'
YELLOW = '\033[93m'
BLUE = '\033[94m'
RESET = '\033[0m'

def log(msg, color=None):
    if color:
        print(f"{color}{msg}{RESET}")
    else:
        print(msg)

def main():
    """
    Generate 10 test chains with heights: 5, 10, 15, 20, 25, 30, 35, 40, 45, 50
    Each chain is saved to test/data/chain_N/ where N is the height
    """

    base_output_dir = Path(__file__).parent.parent / "data"
    base_output_dir.mkdir(parents=True, exist_ok=True)

    # Rely on TestNode auto-resolving unicityd

    log("\n" + "="*70, BLUE)
    log("GENERATING PRE-MINED TEST CHAINS", BLUE)
    log("="*70, BLUE)
    log(f"Output directory: {base_output_dir}\n", YELLOW)

    # Generate chains with heights: 5, 10, 15, 20, 25, 30, 35, 40, 45, 50
    for i in range(1, 11):
        height = i * 5
        chain_dir = base_output_dir / f"chain_{height}"

        # Remove existing chain if present
        if chain_dir.exists():
            shutil.rmtree(chain_dir)

        log(f"Generating chain with height {height}...", BLUE)

        # Create temporary directory for mining
        temp_dir = Path(tempfile.mkdtemp(prefix=f'chain_{height}_'))

        try:
        # Start node
        node = TestNode(0, temp_dir)
        node.start()

            # Mine blocks
            log(f"  Mining {height} blocks...", YELLOW)
            result = node.generate(height, timeout=120)
            log(f"  Generated {len(result)} block hashes", YELLOW)
            time.sleep(1)  # Let blocks settle

            # Verify (info['blocks'] is the best block height, not total count)
            info = node.get_info()
            expected_height = height  # After mining N blocks, best block height should be N
            log(f"  Current height: {info['blocks']}, expected: {expected_height}", YELLOW)
            if info['blocks'] != expected_height:
                log(f"  ERROR: Height mismatch!", RED)
                node.stop()
                return 1

            log(f"  ✓ Mined {height} blocks", GREEN)
            log(f"  ✓ Best hash: {info['bestblockhash']}", GREEN)

            # Stop node cleanly
            node.stop()
            time.sleep(0.5)

            # Copy datadir to output location
            shutil.copytree(temp_dir, chain_dir)
            log(f"  ✓ Saved to {chain_dir}\n", GREEN)

        finally:
            # Clean up temp directory
            if temp_dir.exists():
                shutil.rmtree(temp_dir)

    log("="*70, GREEN)
    log("ALL TEST CHAINS GENERATED SUCCESSFULLY", GREEN)
    log("="*70, GREEN)
    log(f"\nGenerated {10} chains at heights: 5, 10, 15, 20, 25, 30, 35, 40, 45, 50", YELLOW)
    log(f"Location: {base_output_dir}/", YELLOW)
    log("\nUsage in tests:", BLUE)
    log("  shutil.copytree(test_data / 'chain_15', test_dir / 'node2')", BLUE)

    return 0

if __name__ == '__main__':
    sys.exit(main())
