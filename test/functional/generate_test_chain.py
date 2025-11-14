#!/usr/bin/env python3
"""Generate a pre-mined test chain for batching tests.

This script mines a large chain once and saves it to disk.
The saved chain can then be loaded quickly for testing batching behavior.
"""

import sys
import tempfile
import shutil
import time
from pathlib import Path

# Add test framework to path
sys.path.insert(0, str(Path(__file__).parent / "test_framework"))

from test_node import TestNode


def main():
    """Generate and save test chain."""
    import argparse

    parser = argparse.ArgumentParser(description='Generate pre-mined test chain')
    parser.add_argument('num_blocks', type=int, nargs='?', default=2500,
                       help='Number of blocks to generate (default: 2500)')
    parser.add_argument('output_dir', type=str, nargs='?',
                       default=str(Path(__file__).parent / "test_chains"),
                       help='Output directory for chain data')
    parser.add_argument('-f', '--force', action='store_true',
                       help='Overwrite existing chain without prompting')

    args = parser.parse_args()
    num_blocks = args.num_blocks
    output_dir = Path(args.output_dir)

    print(f"Generating test chain with {num_blocks} blocks...")
    print(f"Output directory: {output_dir}")
    print()

    # Create output directory
    output_dir.mkdir(parents=True, exist_ok=True)
    chain_dir = output_dir / f"chain_{num_blocks}_blocks"

    if chain_dir.exists():
        if args.force:
            print(f"Removing existing chain at {chain_dir}...")
            shutil.rmtree(chain_dir)
        else:
            print(f"Chain already exists at {chain_dir}")
            if sys.stdin.isatty():
                response = input("Overwrite? (y/N): ")
                if response.lower() != 'y':
                    print("Aborted.")
                    return 0
            else:
                print("Error: Chain exists and --force not specified (non-interactive mode)")
                print("Use --force to overwrite without prompting")
                return 1
            shutil.rmtree(chain_dir)

    # Setup temporary directory for node
    test_dir = Path(tempfile.mkdtemp(prefix="unicity_gen_"))

    node = None

    try:
        print("Starting node...")
        # Rely on TestNode auto-resolving unicityd; bind a fixed port to avoid conflicts
        node = TestNode(0, test_dir / "node0", extra_args=["--listen", "--port=19500"])
        node.start()

        time.sleep(2)

        # Mine blocks in chunks
        print(f"\nMining {num_blocks} blocks...")
        print("This will take approximately {:.1f} minutes...".format(num_blocks * 0.7 / 60))
        print()

        chunk_size = 10  # Smaller chunks to work with 500ms delay fix
        start_time = time.time()

        for i in range(0, num_blocks, chunk_size):
            blocks_to_mine = min(chunk_size, num_blocks - i)
            node.generate(blocks_to_mine, timeout=300)
            info = node.get_info()

            elapsed = time.time() - start_time
            blocks_done = info['blocks']
            blocks_remaining = num_blocks - blocks_done
            time_per_block = elapsed / blocks_done if blocks_done > 0 else 0.2
            eta = blocks_remaining * time_per_block

            print(f"  Progress: {blocks_done}/{num_blocks} blocks "
                  f"({100*blocks_done/num_blocks:.1f}%) - "
                  f"Elapsed: {elapsed/60:.1f}m, ETA: {eta/60:.1f}m")

        # Verify final height
        info = node.get_info()
        assert info['blocks'] == num_blocks, f"Expected {num_blocks} blocks, got {info['blocks']}"

        total_time = time.time() - start_time
        print(f"\n✓ Mining complete in {total_time/60:.1f} minutes!")
        print(f"  Final height: {info['blocks']}")
        print(f"  Tip: {info['bestblockhash'][:16]}...")

        # Stop node to ensure data is flushed
        print("\nStopping node...")
        node.stop()
        time.sleep(1)

        # Copy the chain data to output directory
        print(f"\nSaving chain to {chain_dir}...")
        shutil.copytree(test_dir / "node0", chain_dir)

        # Create metadata file
        metadata = {
            "blocks": num_blocks,
            "tip": info['bestblockhash'],
            "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
            "time_taken_seconds": int(total_time)
        }

        import json
        with open(chain_dir / "chain_metadata.json", 'w') as f:
            json.dump(metadata, f, indent=2)

        print(f"\n✓ Test chain saved successfully!")
        print(f"  Location: {chain_dir}")
        print(f"  Size: {sum(f.stat().st_size for f in chain_dir.rglob('*') if f.is_file()) / (1024*1024):.1f} MB")
        print(f"\nTo use this chain in tests, copy it to your test node's datadir.")

    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
        return 1
    except Exception as e:
        print(f"\n✗ Failed: {e}")
        import traceback
        traceback.print_exc()
        return 1
    finally:
        # Cleanup
        if node and node.is_running():
            node.stop()

        print(f"\nCleaning up temporary directory: {test_dir}")
        shutil.rmtree(test_dir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
