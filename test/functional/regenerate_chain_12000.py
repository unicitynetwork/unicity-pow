#!/usr/bin/env python3
"""Regenerate just the 12000-block chain with increased timeout."""

import sys
import time
import tempfile
import shutil
import json
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "test_framework"))
from test_node import TestNode

def main():
    print("\n" + "="*70)
    print("Generating 12000-block chain with extended timeout")
    print("="*70)
    
    test_dir = Path(tempfile.mkdtemp(prefix=f"gen_12000_"))

    node = TestNode(0, test_dir / "node0")
    try:
        print("Starting node...")
        node.start()
        time.sleep(1)
        
        print(f"Mining 12000 blocks with 5-minute timeout per batch...")
        start = time.time()
        
        # Mine in batches with longer timeout
        batch_size = 100
        for i in range(0, 12000, batch_size):
            remaining = min(batch_size, 12000 - i)
            # Use 300 second (5 minute) timeout per batch
            node.generate(remaining, timeout=300)
            print(f"  Progress: {i + remaining}/12000 blocks ({int((i + remaining) / 12000 * 100)}%)")
        
        elapsed = time.time() - start
        print(f"Mining complete in {elapsed:.1f}s ({elapsed/60:.1f} minutes)")
        
        info = node.get_info()
        print(f"Final height: {info['blocks']}")
        print(f"Tip: {info['bestblockhash'][:16]}...")
        
        node.stop()
        time.sleep(1)
        
        # Save to test_chains
        output_dir = Path(__file__).parent / "test_chains" / "chain_12000_blocks"
        if output_dir.exists():
            shutil.rmtree(output_dir)
        output_dir.parent.mkdir(parents=True, exist_ok=True)
        
        print(f"Saving chain to {output_dir}...")
        shutil.copytree(test_dir / "node0", output_dir)
        
        # Verify
        print("Verifying saved chain...")
        with open(output_dir / "headers.json") as f:
            data = json.load(f)
            saved_height = data['block_count'] - 1
            print(f"Saved {saved_height} blocks (+ genesis = {saved_height + 1} total)")
            assert saved_height == 12000, f"Height mismatch: expected 12000, got {saved_height}"
        
        print(f"\n✓ Successfully generated chain_12000_blocks")
        return 0
        
    except Exception as e:
        print(f"\n✗ Error: {e}")
        import traceback
        traceback.print_exc()
        return 1
    finally:
        if node:
            node.stop()
        # Clean up temp dir
        shutil.rmtree(test_dir, ignore_errors=True)

if __name__ == "__main__":
    sys.exit(main())
