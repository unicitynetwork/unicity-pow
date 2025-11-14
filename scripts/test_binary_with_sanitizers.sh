#!/bin/bash
# Test the main unicity binary with sanitizers
#
# This script builds and runs the main unicity binary with each sanitizer
# to catch bugs that might only manifest in the real application (not unit tests)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Default test duration (seconds)
DURATION=${1:-10}

usage() {
    echo "Usage: $0 [duration_seconds]"
    echo ""
    echo "Tests the main unicity binary with all sanitizers"
    echo ""
    echo "Arguments:"
    echo "  duration_seconds    How long to run each test (default: 10)"
    echo ""
    echo "Examples:"
    echo "  $0              # Run for 10 seconds with each sanitizer"
    echo "  $0 30           # Run for 30 seconds with each sanitizer"
    exit 1
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    usage
fi

test_with_sanitizer() {
    local sanitizer=$1
    local duration=$2

    echo -e "${YELLOW}========================================${NC}"
    echo -e "${YELLOW}Testing with ${sanitizer} sanitizer${NC}"
    echo -e "${YELLOW}========================================${NC}"

    BUILD_DIR="${PROJECT_ROOT}/build_${sanitizer}"

    # Build
    echo "Building unicityd with ${sanitizer} sanitizer..."
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    if ! cmake -DSANITIZER="${sanitizer}" -DCMAKE_BUILD_TYPE=Debug .. > /dev/null 2>&1; then
        echo -e "${RED}✗ CMake configuration failed${NC}"
        return 1
    fi

    if ! cmake --build . --target unicityd -j8 2>&1 | tail -5; then
        echo -e "${RED}✗ Build failed${NC}"
        return 1
    fi

    # Run test
    echo ""
    echo "Running unicityd for ${duration} seconds with ${sanitizer}..."
    DATADIR="/tmp/unicity_${sanitizer}_test"
    rm -rf "$DATADIR"

    # Capture output
    OUTPUT=$(timeout ${duration}s ./bin/unicityd --regtest --datadir="$DATADIR" 2>&1 || true)

    # Check for sanitizer warnings
    if echo "$OUTPUT" | grep -q "WARNING: ${sanitizer^}Sanitizer"; then
        echo -e "${RED}✗ ${sanitizer} detected issues:${NC}"
        echo "$OUTPUT" | grep -A 20 "WARNING: ${sanitizer^}Sanitizer"
        return 1
    fi

    # Check for crashes
    if echo "$OUTPUT" | grep -q "SUMMARY:.*Sanitizer"; then
        echo -e "${RED}✗ ${sanitizer} found errors:${NC}"
        echo "$OUTPUT" | grep -A 10 "SUMMARY:"
        return 1
    fi

    echo -e "${GREEN}✓ ${sanitizer} passed (no issues detected)${NC}"
    return 0
}

# Main
cd "$PROJECT_ROOT"

echo "========================================="
echo "Testing unicityd binary with sanitizers"
echo "Duration: ${DURATION} seconds per test"
echo "========================================="
echo ""

FAILED=0

test_with_sanitizer "thread" "$DURATION" || FAILED=$((FAILED + 1))
echo ""

test_with_sanitizer "address" "$DURATION" || FAILED=$((FAILED + 1))
echo ""

test_with_sanitizer "undefined" "$DURATION" || FAILED=$((FAILED + 1))
echo ""

echo "========================================="
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ All sanitizer tests passed!${NC}"
    echo ""
    echo "The unicityd binary ran for ${DURATION} seconds with each sanitizer"
    echo "and no memory errors, data races, or undefined behavior was detected."
    exit 0
else
    echo -e "${RED}✗ $FAILED sanitizer test(s) failed${NC}"
    exit 1
fi
