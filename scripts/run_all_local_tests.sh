#!/bin/bash
# run_all_local_tests.sh — Comprehensive local test suite for mqvpn
#
# This script runs all GitHub Actions tests locally:
#   1. Unit tests: Control API (get_status), pending path regression
#   2. E2E tests: Path API operations (add/remove/list_paths/get_status)
#
# All tests use the same build and namespace infrastructure for consistency.
# Useful for pre-commit validation or local CI debugging.
#
# Usage: sudo ./scripts/run_all_local_tests.sh [options]
# Options:
#   --build-only      Only build, don't run tests
#   --tests-only      Skip build, run tests only  
#   --help            Show this help
#
# Requires: root, cmake, ninja-build, gcc, openssl, libevent-dev, iproute2, netcat

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
WORK_DIR="${REPO_ROOT}/.test-work"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test results tracking
TESTS_PASSED=0
TESTS_FAILED=0
TEST_RESULTS=()

# Parse arguments
BUILD_ONLY=0
TESTS_ONLY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-only) BUILD_ONLY=1; shift ;;
        --tests-only) TESTS_ONLY=1; shift ;;
        --help) echo "Usage: $0 [--build-only|--tests-only]"; exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Helpers ──────────────────────────────────────────────────────────────────

log_info() {
    echo -e "${BLUE}[INFO]${NC} $*"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $*"
    TESTS_PASSED=$((TESTS_PASSED + 1))
    TEST_RESULTS+=("${GREEN}PASS${NC}: $*")
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $*"
    TESTS_FAILED=$((TESTS_FAILED + 1))
    TEST_RESULTS+=("${RED}FAIL${NC}: $*")
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

die() {
    echo -e "${RED}[FATAL]${NC} $*" >&2
    exit 1
}

# ── Root check ───────────────────────────────────────────────────────────────

if [ "$EUID" -ne 0 ]; then 
    die "This script requires root permissions (for namespace operations)"
fi

# ── Build stage ──────────────────────────────────────────────────────────────

if [ "$TESTS_ONLY" -ne 1 ]; then
    echo ""
    echo "================================================================"
    echo "  = BUILD STAGE"
    echo "================================================================"
    
    log_info "Checking if we need to rebuild..."
    
    # Check if build already exists
    if [ -f "$BUILD_DIR/CMakeCache.txt" ] && \
       [ -f "$BUILD_DIR/tests/test_server" ] && \
       [ -f "$BUILD_DIR/tests/test_pending_path" ] && \
       [ -f "$BUILD_DIR/tests/test_path_readd_reconnect" ] && \
       [ -f "$BUILD_DIR/mqvpn" ]; then
        log_info "Using existing build directory at $BUILD_DIR"
    else
        log_info "Creating build directory..."
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"

        log_info "Running cmake..."
        cmake -DCMAKE_BUILD_TYPE=Release .. || die "cmake failed"

        log_info "Building mqvpn and test binaries..."
        make -j"$(nproc)" test_server test_pending_path test_path_readd_reconnect mqvpn \
              || die "build failed"

        cd "$REPO_ROOT"
    fi

    if [ ! -f "$BUILD_DIR/tests/test_server" ]; then
        die "test_server binary not found after build"
    fi
    if [ ! -f "$BUILD_DIR/tests/test_pending_path" ]; then
        die "test_pending_path binary not found after build"
    fi
    if [ ! -f "$BUILD_DIR/tests/test_path_readd_reconnect" ]; then
        die "test_path_readd_reconnect binary not found after build"
    fi
    if [ ! -f "$BUILD_DIR/mqvpn" ]; then
        die "mqvpn binary not found after build"
    fi
    
    log_pass "Build complete"
fi

if [ "$BUILD_ONLY" -eq 1 ]; then
    echo ""
    echo "Build-only mode; exiting now."
    exit 0
fi

# ── Test stage ───────────────────────────────────────────────────────────────

echo ""
echo "================================================================"
echo "  = TEST STAGE"
echo "================================================================"

# Cleanup on exit
cleanup_tests() {
    log_info "Running cleanup..."
    # Clean up namespaces created by E2E tests
    ip netns del vpn-server-pa 2>/dev/null || true
    ip netns del vpn-client-pa 2>/dev/null || true
    # Clean up work directories
    rm -rf "$WORK_DIR" 2>/dev/null || true
}
trap cleanup_tests EXIT

# Create work directory
mkdir -p "$WORK_DIR"

# ── Helper: run a unit test binary ──────────────────────────────────────────
# Usage: run_unit_test <label> <binary>

run_unit_test() {
    local label="$1"
    local binary="$2"
    log_info "Running $label..."
    if "$binary" 2>&1; then
        log_pass "Unit test suite: $label"
    else
        log_fail "Unit test suite: $label"
        return 1
    fi
}

# ── Unit Tests ───────────────────────────────────────────────────────────────

echo ""
echo "--- Unit Tests ---"

run_unit_test "test_server (Control API / server lifecycle)" \
    "$BUILD_DIR/tests/test_server"

run_unit_test "test_pending_path (issue #4271 — path slot lifecycle)" \
    "$BUILD_DIR/tests/test_pending_path"

run_unit_test "test_path_readd_reconnect (issue #4276 — bounce / reconnect)" \
    "$BUILD_DIR/tests/test_path_readd_reconnect"

# ── E2E Test: Path API (with namespaces) ─────────────────────────────────────

echo ""
echo "--- E2E Tests: Path API (with namespaces) ---"
log_info "Running E2E path API tests with network namespaces..."

run_e2e_test() {
    local label="$1"
    local script="$2"
    local logfile="${WORK_DIR}/$(basename "$script" .sh).log"
    log_info "Running $label..."
    if [ ! -f "$script" ]; then
        log_fail "$label (script not found: $script)"
        return 1
    fi
    if bash "$script" "$BUILD_DIR/mqvpn" > "$logfile" 2>&1; then
        log_pass "$label"
    else
        log_fail "$label"
        log_warn "Last 30 lines of output:"
        tail -30 "$logfile" || true
        return 1
    fi
}

run_e2e_test "E2E: Path API add/remove/list (#4271/#4273)" \
    "${SCRIPT_DIR}/ci_e2e/run_path_api_test.sh"

run_e2e_test "E2E: Path bounce stability — 10 add/remove cycles (#4276)" \
    "${SCRIPT_DIR}/ci_e2e/run_path_bounce_test.sh"

# ── Test Results Summary ─────────────────────────────────────────────────────

echo ""
echo "================================================================"
echo "  = TEST RESULTS SUMMARY"
echo "================================================================"

TOTAL_TESTS=$((TESTS_PASSED + TESTS_FAILED))
echo ""
for result in "${TEST_RESULTS[@]}"; do
    echo -e "$result"
done

echo ""
echo "Total: $TOTAL_TESTS tests"
echo -e "Passed: ${GREEN}$TESTS_PASSED${NC}"
echo -e "Failed: ${RED}$TESTS_FAILED${NC}"

if [ "$TESTS_FAILED" -eq 0 ]; then
    echo ""
    echo -e "${GREEN}✓ All test suites PASSED${NC}"
    echo ""
    echo "Test Coverage:"
    echo "  ✓ Unit Tests: Control API / server lifecycle (test_server)"
    echo "  ✓ Unit Tests: Path slot lifecycle — issue #4271 (test_pending_path)"
    echo "  ✓ Unit Tests: Bounce / reconnect regression — issue #4276 (test_path_readd_reconnect)"
    echo "  ✓ E2E Tests: Path API operations via network namespaces"
    echo "    - Path activation (Bug 1 + #4273)"
    echo "    - Path removal and tunnel survival (Bug 2)"
    echo "    - list_paths command"
    echo "    - get_status command"
    echo "  ✓ E2E Tests: Path bounce stability — 10 add/remove cycles (#4276)"
    echo "    - Budget exhaustion + forced reconnect"
    echo "    - Path reactivation after reconnect"
    exit 0
else
    echo ""
    echo -e "${RED}✗ Some test suites FAILED${NC}"
    exit 1
fi
