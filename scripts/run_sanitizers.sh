#!/bin/bash
# Sanitizer test runner for Unicity
#
# This script builds and runs tests with different sanitizers to catch:
# - ThreadSanitizer (TSan): Data races, deadlocks
# - AddressSanitizer (ASan): Memory leaks, use-after-free, buffer overflows
# - UndefinedBehaviorSanitizer (UBSan): Undefined behavior

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 [thread|address|undefined|all] [test_pattern]"
    echo ""
    echo "Examples:"
    echo "  $0 thread                    # Run all tests with ThreadSanitizer"
    echo "  $0 address '[lifecycle]'     # Run lifecycle tests with AddressSanitizer"
    echo "  $0 all                       # Run all tests with all sanitizers"
    exit 1
}

run_sanitizer() {
    local sanitizer=$1
    local test_pattern=${2:-""}

    echo -e "${YELLOW}========================================${NC}"
    echo -e "${YELLOW}Running ${sanitizer} sanitizer${NC}"
    echo -e "${YELLOW}========================================${NC}"

    BUILD_DIR="${PROJECT_ROOT}/build_${sanitizer}"

    # Clean and configure
    echo "Configuring build with ${sanitizer} sanitizer..."
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    if ! cmake -DSANITIZER="${sanitizer}" -DCMAKE_BUILD_TYPE=Debug .. > /dev/null 2>&1; then
        echo -e "${RED}✗ CMake configuration failed${NC}"
        return 1
    fi

    # Build
    echo "Building tests..."
    if ! cmake --build . --target unicity_tests -j8 2>&1 | tail -5; then
        echo -e "${RED}✗ Build failed${NC}"
        return 1
    fi

    # Run tests
    echo "Running tests..."
    if [ -n "$test_pattern" ]; then
        ./bin/unicity_tests "$test_pattern"
    else
        ./bin/unicity_tests
    fi

    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}✓ ${sanitizer} tests passed${NC}"
    else
        echo -e "${RED}✗ ${sanitizer} tests failed (exit code: $exit_code)${NC}"
        return 1
    fi

    return 0
}

# Main
SANITIZER=${1:-""}
TEST_PATTERN=${2:-""}

if [ -z "$SANITIZER" ]; then
    usage
fi

if [ "$SANITIZER" = "all" ]; then
    FAILED=0

    run_sanitizer "thread" "$TEST_PATTERN" || FAILED=$((FAILED + 1))
    run_sanitizer "address" "$TEST_PATTERN" || FAILED=$((FAILED + 1))
    run_sanitizer "undefined" "$TEST_PATTERN" || FAILED=$((FAILED + 1))

    echo ""
    echo "========================================="
    if [ $FAILED -eq 0 ]; then
        echo -e "${GREEN}✓ All sanitizers passed${NC}"
        exit 0
    else
        echo -e "${RED}✗ $FAILED sanitizer(s) failed${NC}"
        exit 1
    fi
elif [ "$SANITIZER" = "thread" ] || [ "$SANITIZER" = "address" ] || [ "$SANITIZER" = "undefined" ]; then
    run_sanitizer "$SANITIZER" "$TEST_PATTERN"
else
    echo -e "${RED}Unknown sanitizer: $SANITIZER${NC}"
    usage
fi
