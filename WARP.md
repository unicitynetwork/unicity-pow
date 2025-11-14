# WARP.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

## Project Overview

**Unicity (Unicity PoW)** is a headers-only blockchain implementation with no transaction processing. It uses RandomX proof-of-work and ASERT difficulty adjustment, designed for timestamping services and decentralized ordering.

Key characteristics:
- **Headers-only**: 100-byte headers, no UTXO set or transaction processing
- **RandomX PoW**: ASIC-resistant memory-hard algorithm requiring ~2GB RAM
- **ASERT difficulty**: Per-block adjustment targeting 1-hour block times
- **Bitcoin-compatible P2P**: 98% wire protocol compatibility with strategic deviations

## Local Development: `tmp/` Directory

**Use `/tmp/` for local-only analysis and documentation that should NOT be committed.**

This directory is gitignored for:
- Vulnerability analysis documents
- Performance test results
- Code review notes
- Proof-of-concept exploits
- WIP documentation
- Debug output and logs

**Example**:
```bash
# Store analysis
echo "## CPU DoS via checksum validation" > tmp/analysis.md

# Run tests and save results
./build/bin/unicity_tests "[dos]" > tmp/dos_test_results.txt

# Verify nothing will be committed
git status  # tmp/ won't appear
```

See `tmp/README.md` and `tmp/QUICK_START.md` for details.

---

## Build & Development Commands

### Initial Setup
```bash
# Build from source
mkdir build && cd build
cmake ..
make -j$(nproc)

# Executables are placed in build/bin/
./bin/unicityd       # Main node
./bin/unicity-cli    # RPC client
```

### Testing
```bash
# Run all tests (357 test cases, 4,806 assertions)
./build/unicity_tests

# Run with verbose output
./build/unicity_tests -v

# Run with trace-level logging for debugging
COINBASE_TEST_LOG_LEVEL=trace ./build/unicity_tests

# Run specific test category with trace logging
COINBASE_TEST_LOG_LEVEL=trace ./build/unicity_tests "[dos]"

# Run specific test categories (using Catch2 tags)
./build/unicity_tests "[unit]"         # Unit tests
./build/unicity_tests "[integration]"  # Integration tests
./build/unicity_tests "[network]"      # Network/P2P tests
./build/unicity_tests "[security]"     # Security/DoS tests
./build/unicity_tests "[randomx]"      # RandomX PoW tests

# Run specific test case by name
./build/unicity_tests "MessageRouter"

# Run test and save trace output
COINBASE_TEST_LOG_LEVEL=trace ./build/unicity_tests "[dos][flood]" > tmp/dos_trace.log 2>&1

# Show test timings
./build/unicity_tests --durations yes

# List all available tests
./build/unicity_tests --list-tests

# JUnit output for CI
./build/unicity_tests -r junit -o test-results.xml
```

### Docker Commands
```bash
# Build Docker image
docker build -f deploy/docker/Dockerfile -t unicity:latest .
# OR use the symlinked Dockerfile at root
docker build -f Dockerfile -t unicity:latest .

# Using Makefile shortcuts
make build              # Build main image
make build-test         # Build test image
make build-all          # Build both
make test               # Run full test suite in Docker
make run                # Run mainnet node
make run-regtest        # Run regtest node
make logs               # View container logs
make cli ARGS="getblockcount"  # Execute RPC commands

# Docker Compose
docker-compose up -d    # Start node
docker-compose down     # Stop node
```

### Sanitizer Testing
```bash
# Build with sanitizers (for bug hunting)
mkdir build-tsan && cd build-tsan
cmake -DSANITIZE=thread ..
make -j$(nproc)
./unicity_tests

# Other sanitizers: address, undefined
cmake -DSANITIZE=address ..
```

### Fuzzing
```bash
# Enable fuzzing (requires clang with libFuzzer)
mkdir build-fuzz && cd build-fuzz
cmake -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++ ..
make -j$(nproc)

# Run fuzz targets (located in build-fuzz/fuzz/)
./fuzz/fuzz_block_header
./fuzz/fuzz_messages
./fuzz/fuzz_chain_reorg
```

## Architecture Overview

### Layer Structure
The codebase is organized into two main libraries:

1. **chain library**: All blockchain logic
   - Crypto (SHA256, RandomX, uint256)
   - Primitives (block headers)
   - Consensus (PoW, ASERT difficulty)
   - Validation (header validation, chain selection)
   - Chain management (BlockManager, ChainstateManager)
   - Mining

2. **network library**: All I/O and communication
   - P2P protocol (message framing, peer connections)
   - Network management (PeerManager, AddrManager)
   - Header synchronization (HeaderSyncManager)
   - RPC server/client (Unix socket based)
   - NAT traversal (UPnP)

### Critical Components

**ChainstateManager** (`src/chain/chainstate_manager.cpp`)
- Central validation coordinator
- Handles header acceptance, chain activation, reorganizations
- Entry point: `AcceptBlockHeader()` → `ActivateBestChain()`

**NetworkManager** (`src/network/network_manager.cpp`)
- P2P network orchestrator
- Manages peer connections, message routing, header sync
- Uses Boost.Asio for async I/O with 4-thread pool

**Validation Pipeline** (3 layers):
1. **Pre-validation** (~1ms): Fast PoW commitment check for DoS protection
2. **Context-free** (~50ms): Full RandomX PoW verification
3. **Contextual** (~5ms): ASERT difficulty, timestamp (MTP), chain work

**Block Header Structure** (100 bytes):
```cpp
struct CBlockHeader {
    int32_t nVersion;        // 4 bytes
    uint256 hashPrevBlock;   // 32 bytes
    uint160 minerAddress;    // 20 bytes (replaces merkleRoot)
    uint32_t nTime;          // 4 bytes
    uint32_t nBits;          // 4 bytes (difficulty target)
    uint32_t nNonce;         // 4 bytes
    uint256 hashRandomX;     // 32 bytes (RandomX PoW commitment)
};
```

### Thread Model
- **Main Thread**: RPC server, application logic
- **Network I/O Pool**: 4 threads for async socket operations
- **Validation Thread**: Header validation, chain activation, orphan processing
- **Mining Thread**: Optional, for RandomX hash computation

### Data Storage
```
~/.unicity/
├── blocks/headers.dat       # Serialized block headers
├── chainstate/              # LevelDB chain state
├── peers.dat                # Known peer addresses
├── anchors.json             # Trusted anchor nodes
├── banlist.dat              # Banned peers
└── node.sock                # Unix socket for RPC
```

## Protocol Implementation

### P2P Messages (Bitcoin-compatible)
All messages use 24-byte header: magic (4) + command (12) + length (4) + checksum (4)

**Supported messages**: version, verack, ping, pong, addr, getaddr, inv, getdata, headers, getheaders, notfound

**NOT supported**: tx, block, mempool, getblocks (no full blocks or transactions)

### Network Parameters
- Magic bytes: 0x554E4943 ("UNIC")
- Default P2P port: 9590
- RPC: Unix socket at `datadir/node.sock` (NOT TCP/IP)
- Protocol version: 1
- Max connections: 125 inbound, 8 outbound

### Consensus Rules
- **RandomX PoW**: Hash must be < target
- **ASERT difficulty**: Per-block adjustment with 48-block (2-day) half-life
- **Timestamp validation**: Must be > median(last 11 blocks) and < now + 2 hours
- **Block time**: Target 1 hour (3600 seconds)

## Important Implementation Details

### RPC Design Choice
**This project uses Unix domain sockets, NOT TCP/IP for RPC.** This is a security feature:
- Local-only access (no network exposure)
- No authentication needed (file system permissions)
- Socket location: `datadir/node.sock`
- No `rpcport`, `rpcbind`, or `rpcallowip` options exist

For remote monitoring, SSH to the server and run `unicity-cli` locally.

### RandomX Memory Requirements
- 2GB dataset cached in memory when mining or validating
- Fast mode: ~2.2GB RAM total
- Light mode: Not currently implemented

### Security Features
- DoS protection via work threshold (GetAntiDoSWorkThreshold)
- Orphan limits: 1000 global, 50 per peer
- Misbehavior scoring: 100 points = instant ban
- Connection rate limiting: Max 125 inbound
- Receive buffer limit: 5MB per connection

### Testing Framework
Uses **Catch2** for all tests (not Google Test). Test organization:
- Tags like `[unit]`, `[network]`, `[security]` categorize tests
- Simulated network for deterministic P2P testing (`test/network/simulated_network.cpp`)
- Comprehensive coverage: 93% overall, 357 test cases

## Code Style Patterns

### Naming Conventions
- Classes: `CamelCase` (e.g., `CBlockHeader`, `ChainstateManager`)
- Functions: `CamelCase` for class methods (e.g., `AcceptBlockHeader`)
- Variables: `snake_case` (e.g., `block_index`, `peer_manager`)
- Member variables: `snake_case` with trailing `_` (e.g., `chainstate_manager_`)
- Constants: `SCREAMING_SNAKE_CASE` (e.g., `MAX_PROTOCOL_MESSAGE_LENGTH`)

### Error Handling
- Validation functions return `bool` or `ValidationState`
- Network errors logged via spdlog but don't crash
- Critical consensus failures throw exceptions

### Logging
Uses **spdlog** with categories:
- `net`: Network and P2P events
- `validation`: Consensus and chain validation
- `bench`: Performance measurements

Enable with: `./unicityd -debug=net,validation`

## Common Development Patterns

### Adding New P2P Message Type
1. Add command string constant in `include/network/protocol.hpp`
2. Define message struct in `include/network/message.hpp`
3. Implement serialization in `src/network/message.cpp`
4. Add handler in `MessageRouter` (`src/network/message_router.cpp`)
5. Add tests in `test/unit/message_tests.cpp` and `test/network/`

### Modifying Consensus Rules
1. Update validation logic in `src/chain/validation.cpp`
2. Update chain selector if needed (`src/chain/chain_selector.cpp`)
3. Add comprehensive tests in `test/unit/validation_tests.cpp`
4. Update protocol docs in `docs/PROTOCOL_SPECIFICATION.md`
5. Run full test suite to verify no regressions

### Adding RPC Command
1. Add command handler in `src/network/rpc_server.cpp`
2. Update help text in same file
3. Add tests if complex logic involved
4. Document in README.md under RPC Interface section

## Key Files Reference

### Entry Points
- `src/main.cpp`: Node startup and initialization
- `src/cli.cpp`: RPC client implementation
- `src/application.cpp`: Main application logic and coordination

### Core Headers
- `include/chain/block.hpp`: Block header definition
- `include/chain/chainstate_manager.hpp`: Main validation interface
- `include/network/network_manager.hpp`: P2P network manager
- `include/network/protocol.hpp`: Protocol constants and messages

### Important Source Files
- `src/chain/validation.cpp`: Core validation logic (CheckBlockHeader, etc.)
- `src/chain/pow.cpp`: ASERT difficulty calculation
- `src/chain/randomx_pow.cpp`: RandomX integration
- `src/network/peer.cpp`: Peer connection handling
- `src/network/header_sync_manager.cpp`: Header synchronization

### Configuration
- `CMakeLists.txt`: Build configuration
- `deploy/docker/Dockerfile`: Docker build definition
- `deploy/ansible/`: Ansible playbooks for deployment

## Documentation Structure

Primary docs in `docs/` directory:
- `ARCHITECTURE.md`: Detailed system architecture
- `PROTOCOL_SPECIFICATION.md`: Complete wire protocol spec
- `PROTOCOL_DEVIATIONS.md`: Differences from Bitcoin
- `DOCUMENTATION_INDEX.md`: Guide to all documentation

## Deployment Notes

### Production Deployment
Uses Ansible for multi-node deployment:
```bash
cd deploy/ansible
ansible-playbook -i inventory.yml deploy-simple.yml
```

Network consists of 6 production nodes (see `deploy/ansible/inventory.yml`)

### Resource Requirements
- Memory: 3GB minimum (2GB for RandomX + overhead)
- CPU: 2+ cores recommended
- Disk: ~1GB for blockchain data (grows <1MB/year)
- Network: Port 9590 must be accessible

## Development Workflow

1. **Make changes** in appropriate source files
2. **Build**: `make -j$(nproc)` from `build/` directory
3. **Test**: Run relevant test suite first
4. **Full test**: Run `./unicity_tests` before committing
5. **Format**: Code follows existing style (no auto-formatter configured)
6. **Document**: Update protocol docs if consensus/protocol changes

## Key Architectural Decisions

1. **Headers-only design**: Eliminates transaction complexity entirely
2. **RandomX over SHA256**: Democratizes mining (CPU-friendly, ASIC-resistant)
3. **ASERT over Bitcoin's 2016-block adjustment**: Smooth per-block difficulty
4. **Unix socket RPC**: Enhanced security over TCP/IP
5. **98% Bitcoin P2P compatibility**: Reuses proven networking patterns

## Related Documentation

- Architecture: `docs/ARCHITECTURE.md`
- Protocol spec: `docs/PROTOCOL_SPECIFICATION.md`
- Testing: See test categories in README.md
- Deployment: `deploy/README.md`
- Docker: `docs/DOCKER.md`
