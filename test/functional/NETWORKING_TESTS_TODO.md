# Networking Tests TODO

This document tracks P2P/networking tests that should be implemented.

## High Priority - DoS Protection & Security

### p2p_dos_headers.py
Test misbehavior scoring system and automatic disconnection:
- ✓ Send headers with invalid PoW → instant disconnect (score 100)
- ✓ Send non-continuous headers → penalty (score 20)
- ✓ Send oversized headers messages → penalty (score 20)
- ✓ Send low-work header spam → penalty (score 10)
- ✓ Verify peer is disconnected at threshold (score ≥ 100)
- ✓ Test that misbehavior score accumulates correctly
- ✓ Verify misbehaving peer cannot reconnect (discouraged)

**Why important**: DoS protection is critical for production. Attackers will try to crash/slow nodes.

### p2p_ban.py
Test ban/discourage system (BanMan):
- ✓ Manual ban via RPC (ban/unban commands)
- ✓ Ban persistence across restarts (banlist.json)
- ✓ Ban expiry (timed bans with mocktime)
- ✓ Permanent bans (ban_time_offset = 0)
- ✓ Discourage (automatic soft-ban for 24h after misbehavior)
- ✓ NoBan permission flag (whitelisted peers can't be banned)
- ✓ Verify banned peer connection rejected
- ✓ Sweep expired bans

**Why important**: Prevents repeat attacks. Persistent bans protect against known bad actors.

## Medium Priority - Connection Management

### p2p_max_connections.py
Test connection limits and eviction:
- ✓ Max outbound peers (8) - verify can't exceed
- ✓ Max inbound peers (125) - verify can't exceed
- ✓ Eviction when at capacity (evict worst ping time)
- ✓ Eviction protects recent connections (10 second window)
- ✓ Eviction protects outbound connections (never evict)
- ✓ Eviction protects NoBan/Manual connections
- ✓ Test eviction tie-breaker (oldest connection)

**Why important**: Ensures nodes don't get overloaded. Fair resource allocation.

### p2p_disconnect.py
Test disconnection scenarios:
- ✓ Graceful disconnect (clean shutdown)
- ✓ Peer reconnection after disconnect
- ✓ Self-connection prevention (nonce matching)
  - Outbound: detected by NetworkManager before connection
  - Inbound: detected by Peer after VERSION message
- ✓ Connection cleanup (resources freed)
- ✓ Verify no memory leaks on rapid connect/disconnect

**Why important**: Robust disconnect handling prevents resource leaks.

### p2p_inactivity_timeout.py
Test timeout-based disconnections (uses mocktime):
- ✓ Inactivity timeout (20 minutes) - peer sends no messages
- ✓ Ping timeout (20 minutes) - peer doesn't respond to PING
- ✓ Handshake timeout (60 seconds) - peer doesn't complete VERSION/VERACK
- ✓ Verify timeout checks use mockable time
- ✓ Test that activity resets timeout counters

**Why important**: Prevents zombie connections. Frees resources for active peers.

## Medium Priority - Address Management

### p2p_addr_relay.py
Test address announcement and relay:
- ✓ ADDR message sending/receiving
- ✓ Address manager storage (new vs tried)
- ✓ Address relay rate limiting (1 per second)
- ✓ Address selection for outbound connections
- ✓ Prefer tried addresses over new addresses
- ✓ Address bucketing (collision resistance)

**Why important**: Peer discovery is essential for network health.

### p2p_addr_manager.py
Test AddressManager internals:
- ✓ Add addresses (new bucket)
- ✓ Mark address as tried (after successful connection)
- ✓ Good/failed tracking
- ✓ Select addresses for connection attempts
- ✓ Persistence (peers.json)
- ✓ Cleanup stale addresses

**Why important**: Robust peer discovery ensures network remains connected.

## Lower Priority - Network Behavior

### p2p_network_split.py
Test chain reorganization after network partition:
- ✓ Split network (two separate chains develop)
- ✓ Rejoin network (nodes connect across partition)
- ✓ Verify longest chain wins (chain with most work)
- ✓ Verify all nodes converge to same chain
- ✓ Test with different chain lengths (6 blocks vs 10 blocks)

**Why important**: Validates consensus rules work correctly during partitions.

### p2p_headers_sync.py
Test header synchronization edge cases:
- ✓ Sync from genesis
- ✓ Sync with large height difference (1000+ blocks)
- ✓ Sync with checkpoint verification
- ✓ Handle headers in wrong order
- ✓ Handle duplicate headers
- ✓ Handle headers from multiple peers simultaneously
- ✓ Verify header batching (2000 headers per message)

**Why important**: Header sync is core functionality. Must handle all edge cases.

### p2p_reorg.py
Test chain reorganization:
- ✓ Simple reorg (5 block rollback)
- ✓ Deep reorg (100+ block rollback)
- ✓ Verify block index updates correctly
- ✓ Verify chain work calculations
- ✓ Test reorg with mining in progress

**Why important**: Reorgs happen naturally. Must handle gracefully.

## Lower Priority - Stress Testing

### p2p_stress.py
High-load scenarios:
- ✓ Many simultaneous connections (100+ peers)
- ✓ Rapid connect/disconnect cycles
- ✓ Large header messages at max size (2000 headers)
- ✓ Verify no memory leaks under load
- ✓ Verify no deadlocks
- ✓ Monitor CPU/memory usage

**Why important**: Production nodes will face high load. Must remain stable.

### p2p_fuzz.py
Fuzzing/malformed messages:
- ✓ Invalid message headers (bad magic, bad checksum)
- ✓ Truncated messages
- ✓ Messages with invalid lengths
- ✓ Invalid VERSION message fields
- ✓ Verify node doesn't crash on any input

**Why important**: Attackers will send malformed data. Must handle safely.

## Current Test Status

### Existing Tests (Already Implemented)
- ✓ p2p_connect.py - Basic 2-node connection and block propagation
- ✓ p2p_three_nodes.py - 3-node network topology
- ✓ p2p_ibd.py - Initial Block Download
- ✓ p2p_batching.py - Header batching (2000 headers per message)
- ✓ p2p_eviction.py - Mocktime system for timeout testing

## Implementation Notes

### Testing Requirements
- All tests should use mocktime where applicable (timeouts, expiry)
- Tests should verify both success cases and failure cases
- Tests should check log messages for expected warnings/errors
- Tests should clean up resources (stop nodes, remove test dirs)

### Test Framework Improvements Needed
- Need getpeerinfo RPC to return real peer data (currently returns dummy data)
- Need setban/listbanned/clearbanned RPC commands
- Need to expose misbehavior scores via RPC (for testing)
- Need peer connection count in getinfo

### Priority Order (Recommended)
1. p2p_dos_headers.py (DoS protection is critical)
2. p2p_ban.py (Ban system must work correctly)
3. p2p_inactivity_timeout.py (Complete timeout testing)
4. p2p_max_connections.py (Resource management)
5. p2p_disconnect.py (Robust disconnect handling)
6. Rest as needed for production readiness

## Notes
- This is a headers-only chain, so no transaction relay tests needed
- Focus on header sync, connection management, and DoS protection
- Many tests will require RPC enhancements to expose internal state
