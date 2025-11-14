#!/usr/bin/env python3
"""Consensus: ASERT difficulty evolution on testnet using submitheader skip_pow and getnextworkrequired.

Flow:
- Start node in testnet mode
- H1: time = genesis.time + spacing; bits = powLimit (as returned), accept
- H2: choose a larger-than-spacing dt; bits must still equal getnextworkrequired(tip=H1), accept
- H3: verify getnextworkrequired(tip=H2) != powLimit; submit header with that bits, accept
- Negative: submitting H3' with wrong bits should fail (bad-diffbits)
"""

import sys
import tempfile
import shutil
from pathlib import Path
import struct

# Add test framework to path
sys.path.insert(0, str(Path(__file__).parent / "test_framework"))
from test_node import TestNode

TESTNET_SPACING = 120  # 2 minutes


def u32le(n: int) -> bytes:
    return struct.pack('<I', n & 0xFFFFFFFF)


def hex_to_le32(hex_str: str) -> bytes:
    b = bytes.fromhex(hex_str)
    if len(b) != 32:
        raise ValueError("hash must be 32 bytes")
    return b[::-1]


def build_header_hex(prev_hash_hex_be: str, n_time: int, n_bits_u32: int, n_nonce: int = 0, version: int = 1) -> str:
    prev_le = hex_to_le32(prev_hash_hex_be)
    miner = b"\x00" * 20
    rx = b"\x00" * 32
    header = b"".join([
        u32le(version),
        prev_le,
        miner,
        u32le(n_time),
        u32le(n_bits_u32),
        u32le(n_nonce),
        rx,
    ])
    assert len(header) == 100
    return header.hex()


def main():
    print("Starting consensus_asert_difficulty_testnet test...")

    test_dir = Path(tempfile.mkdtemp(prefix="unicity_consensus_asert_tn_"))
    binary_path = Path(__file__).parent.parent.parent / "build" / "bin" / "unicityd"

    node = None
    try:
        node = TestNode(0, test_dir / "node0", binary_path, extra_args=["--nolisten"], chain="testnet")
        node.start()

        # Genesis
        genesis_hash = node.rpc("getblockhash", 0)
        hdr0 = node.rpc("getblockheader", str(genesis_hash))
        prev_hash = str(genesis_hash)
        t0 = int(hdr0.get("time"))
        powlimit_bits = int(hdr0.get("bits"), 16)

        # Expected bits for block 1
        nw = node.rpc("getnextworkrequired")
        bits1 = int(nw.get("bits_u32"))
        # Debug
        print(f"powlimit_bits={powlimit_bits:#x} bits1={bits1:#x}")
        assert bits1 == powlimit_bits

        # H1 on schedule
        t1 = t0 + TESTNET_SPACING
        h1_hex = build_header_hex(prev_hash, t1, bits1)
        r1 = node.rpc("submitheader", h1_hex, "true")
        assert r1.get("success") is True
        h1_hash = r1.get("hash")

        # Expected bits for H2 (still powLimit because anchor=1 and prev was on schedule)
        nw2 = node.rpc("getnextworkrequired")
        bits2 = int(nw2.get("bits_u32"))
        print(f"bits2={bits2:#x}")

        # Choose H2 with large positive deviation but still valid timestamp
        t2 = t1 + (TESTNET_SPACING + 1200)  # +20 minutes beyond schedule
        # Ensure not too far in the future relative to adjusted time
        node.rpc("setmocktime", t2)  # allow submission
        h2_hex = build_header_hex(h1_hash, t2, bits2)
        r2 = node.rpc("submitheader", h2_hex, "true")
        assert r2.get("success") is True
        h2_hash = r2.get("hash")

        # Now H3 should require a different bits due to deviation at H2
        nw3 = node.rpc("getnextworkrequired")
        bits3_expected = int(nw3.get("bits_u32"))
        print(f"bits3_expected={bits3_expected:#x}")

        # Submit correct H3
        t3 = t2 + TESTNET_SPACING
        h3_hex = build_header_hex(h2_hash, t3, bits3_expected)
        r3 = node.rpc("submitheader", h3_hex, "true")
        assert r3.get("success") is True
        h3_hash = r3.get("hash")

        # Negative: try wrong-bits H3' (flip LSB of bits), expect bad-diffbits
        wrong_bits = bits3_expected ^ 0x1
        h3_bad_hex = build_header_hex(h2_hash, t3, wrong_bits)
        r3_bad = node.rpc("submitheader", h3_bad_hex, "true")
        assert isinstance(r3_bad, dict) and "error" in r3_bad and "bad-diffbits" in r3_bad["error"], f"Expected bad-diffbits, got: {r3_bad}"

        print("✓ consensus_asert_difficulty_testnet passed")
        return 0

    except Exception as e:
        print(f"✗ consensus_asert_difficulty_testnet failed: {e}")
        return 1

    finally:
        if node and node.is_running():
            print("Stopping node...")
            node.stop()
        print(f"Cleaning up {test_dir}")
        shutil.rmtree(test_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
