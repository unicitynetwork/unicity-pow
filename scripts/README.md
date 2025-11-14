# Unicity Static Analysis Scripts

This directory contains static analysis tools to detect memory leaks and other issues.

## Scripts

### check_callback_leaks.py

Detects potential memory leaks caused by callbacks capturing `shared_from_this()`.

**What it checks:**
- Lambdas capturing `shared_from_this()` (potential reference cycles)
- Callback setter methods (`set_*_callback`)
- Cleanup methods and whether they clear callbacks
- Asymmetric cleanup paths (some clear, some don't)

**Usage:**
```bash
# Run from project root
./scripts/check_callback_leaks.py

# Returns 0 if no issues, 1 if issues found
```

**Example output:**
```
=== Analyzing src/network/peer.cpp ===

Found 6 shared_from_this() captures:
  Line 153: variable 'self'
  Line 185: variable 'self'
  ...

Found callback setters:
  connection_: 6 callback(s) set

Found 1 cleanup methods:
  Peer() at line 42: DOES NOT clear callbacks
```

**False Positives:**
- The script may flag destructors that don't manage callbacks (by design)
- Focus on methods that actually set/clear callbacks
- Destructors are defensive-only and don't clear callbacks intentionally

**Integration:**
```bash
# Add to pre-commit hook
./scripts/check_callback_leaks.py || exit 1

# Add to CI/CD
python3 scripts/check_callback_leaks.py
```

## Static Analysis with clang-tidy

The project includes a `.clang-tidy` configuration for additional static analysis.

**Enable in CMake:**
```bash
cmake -DENABLE_CLANG_TIDY=ON ..
cmake --build .
```

**Run standalone:**
```bash
clang-tidy src/network/peer.cpp -- -Iinclude/ -std=c++17
```

## Memory Leak Detection with AddressSanitizer

**Enable ASAN:**
```bash
cmake -DENABLE_ASAN=ON ..
cmake --build .
./build/bin/unicity_tests
```

**Check for leaks:**
```bash
ASAN_OPTIONS=detect_leaks=1 ./build/bin/unicity_tests
```

## Best Practices

When using `shared_from_this()` in callbacks:

** DO:**
- Clear callbacks on ALL cleanup paths (local disconnect, remote disconnect, destructor)
- Use symmetric cleanup (all paths do the same thing)
- Document callback lifecycle in comments
- Test both disconnect scenarios

** DON'T:**
- Leave callbacks intact after disconnect
- Have asymmetric cleanup (some paths clear, others don't)
- Rely on destructors to clear callbacks (too late!)
- Assume callbacks capture `weak_ptr` when they capture `shared_ptr`

## Example: Correct Pattern

```cpp
// Setup (peer.cpp:153)
PeerPtr self = shared_from_this();
connection_->set_receive_callback([self](const std::vector<uint8_t> &data) {
  self->on_transport_receive(data);
});

// Cleanup path 1: local disconnect (peer.cpp:212)
void Peer::do_disconnect() {
  if (connection_) {
    connection_->set_receive_callback({});    // Clear!
    connection_->set_disconnect_callback({}); // Clear!
    connection_->close();
    connection_.reset();
  }
}

// Cleanup path 2: remote disconnect (peer.cpp:369)
void Peer::on_transport_disconnect() {
  if (connection_) {
    connection_->set_receive_callback({});    // Clear!
    connection_->set_disconnect_callback({}); // Clear!
    connection_.reset();
  }
  on_disconnect();
}
```

## Troubleshooting

**Script reports false positives:**
- Focus on files in `src/network/*.cpp`
- Ignore destructor warnings for classes that don't manage callbacks
- Look for actual `set_*_callback` calls

**Script misses issues:**
- Check if callbacks use different naming (not `set_*_callback`)
- Verify cleanup methods are detected (must contain keywords: disconnect, close, cleanup, shutdown, stop)

**No issues found:**
- Good! Keep it that way
- Run after major refactoring
- Add to CI/CD to catch regressions

## Contributing

When adding new callback-based code:
1. Run `./scripts/check_callback_leaks.py` before committing
2. Ensure symmetric cleanup on all paths
3. Document callback lifecycle
4. Add tests for both disconnect scenarios
