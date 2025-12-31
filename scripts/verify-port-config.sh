#!/bin/bash
# =============================================================================
# verify-port-config.sh - Verify port configuration implementation
# =============================================================================
# This script verifies the port migration from 8080 to 9081 is complete.
# Run after building to ensure all configuration paths work correctly.
#
# Usage:
#   ./scripts/verify-port-config.sh [path-to-binary]
#
# See: docs/decisions/DR-001-port-allocation.md
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BINARY="${1:-${PROJECT_ROOT}/build/water-treat}"

PASS_COUNT=0
FAIL_COUNT=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass() {
    echo -e "${GREEN}PASS${NC}: $1"
    ((PASS_COUNT++))
}

fail() {
    echo -e "${RED}FAIL${NC}: $1"
    ((FAIL_COUNT++))
}

warn() {
    echo -e "${YELLOW}WARN${NC}: $1"
}

echo "=============================================="
echo " Port Configuration Verification"
echo "=============================================="
echo ""

# =============================================================================
# Phase 1: Static Analysis - Check source files for stray 8080 references
# =============================================================================
echo "Phase 1: Static Analysis"
echo "------------------------"

# Check C source files
STRAY_C=$(grep -rn "8080" --include="*.c" --include="*.h" "$PROJECT_ROOT/src" 2>/dev/null || true)
if [ -z "$STRAY_C" ]; then
    pass "No hardcoded 8080 in source files (src/*.c, src/*.h)"
else
    fail "Found hardcoded 8080 in source files:"
    echo "$STRAY_C" | head -5
fi

# Check config example
if grep -q "http_port = 9081" "$PROJECT_ROOT/etc/water-treat.conf.example"; then
    pass "Config example uses port 9081"
else
    fail "Config example does not use port 9081"
fi

# Check systemd service uses correct env file
if grep -q "water-treat.env" "$PROJECT_ROOT/systemd/water-treat.service"; then
    pass "Systemd service references water-treat.env"
else
    fail "Systemd service does not reference water-treat.env"
fi

# Check environment file exists and uses 9081
if [ -f "$PROJECT_ROOT/etc/water-treat.env" ]; then
    if grep -q "WT_HTTP_PORT=9081" "$PROJECT_ROOT/etc/water-treat.env"; then
        pass "Environment file sets WT_HTTP_PORT=9081"
    else
        fail "Environment file does not set WT_HTTP_PORT=9081"
    fi
else
    fail "Environment file does not exist"
fi

# Check decision document exists
if [ -f "$PROJECT_ROOT/docs/decisions/DR-001-port-allocation.md" ]; then
    pass "Decision rationale document exists"
else
    fail "Decision rationale document missing"
fi

# Check config_defaults.h exists and defines correct port
if [ -f "$PROJECT_ROOT/include/config_defaults.h" ]; then
    if grep -q "WT_HTTP_PORT_DEFAULT.*9081" "$PROJECT_ROOT/include/config_defaults.h"; then
        pass "config_defaults.h defines WT_HTTP_PORT_DEFAULT=9081"
    else
        fail "config_defaults.h does not define correct default port"
    fi
else
    fail "config_defaults.h does not exist"
fi

echo ""

# =============================================================================
# Phase 2: Runtime Tests (if binary exists)
# =============================================================================
echo "Phase 2: Runtime Tests"
echo "----------------------"

if [ ! -x "$BINARY" ]; then
    warn "Binary not found at $BINARY - skipping runtime tests"
    warn "Build with: mkdir -p build && cd build && cmake .. && make"
else
    echo "Testing binary: $BINARY"

    # Test 1: Default port (expect 9081)
    echo -n "Test 1: Default port resolution... "
    OUTPUT=$(timeout 2 "$BINARY" --test-config 2>&1 || true)
    if echo "$OUTPUT" | grep -q "HTTP Port: 9081"; then
        pass "Default port is 9081"
    else
        fail "Default port is not 9081"
        echo "Output: $OUTPUT" | head -5
    fi

    # Test 2: Environment override
    echo -n "Test 2: Environment override (WT_HTTP_PORT=9000)... "
    OUTPUT=$(WT_HTTP_PORT=9000 timeout 2 "$BINARY" --test-config 2>&1 || true)
    if echo "$OUTPUT" | grep -q "HTTP Port: 9000"; then
        pass "Environment override works"
    else
        fail "Environment override failed"
        echo "Output: $OUTPUT" | head -5
    fi

    # Test 3: CLI override takes precedence
    echo -n "Test 3: CLI override precedence (--http-port 9001)... "
    OUTPUT=$(WT_HTTP_PORT=9000 timeout 2 "$BINARY" --http-port 9001 --test-config 2>&1 || true)
    if echo "$OUTPUT" | grep -q "HTTP Port: 9001"; then
        pass "CLI takes precedence over environment"
    else
        fail "CLI precedence failed"
        echo "Output: $OUTPUT" | head -5
    fi

    # Test 4: Invalid port handling
    echo -n "Test 4: Invalid port rejection (WT_HTTP_PORT=70000)... "
    OUTPUT=$(WT_HTTP_PORT=70000 timeout 2 "$BINARY" --test-config 2>&1 || true)
    # Should either show warning and use default, or reject
    if echo "$OUTPUT" | grep -qi "invalid\|warning\|9081"; then
        pass "Invalid port handled correctly"
    else
        fail "Invalid port not handled"
        echo "Output: $OUTPUT" | head -5
    fi

    # Test 5: Help shows port option
    echo -n "Test 5: Help shows --http-port option... "
    OUTPUT=$(timeout 2 "$BINARY" --help 2>&1 || true)
    if echo "$OUTPUT" | grep -q "\-\-http-port"; then
        pass "Help shows --http-port option"
    else
        fail "Help missing --http-port option"
    fi
fi

echo ""

# =============================================================================
# Summary
# =============================================================================
echo "=============================================="
echo " Summary"
echo "=============================================="
echo ""
echo -e "Passed: ${GREEN}${PASS_COUNT}${NC}"
echo -e "Failed: ${RED}${FAIL_COUNT}${NC}"
echo ""

if [ $FAIL_COUNT -eq 0 ]; then
    echo -e "${GREEN}All checks passed!${NC}"
    exit 0
else
    echo -e "${RED}Some checks failed. Please review the output above.${NC}"
    exit 1
fi
