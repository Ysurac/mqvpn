#!/bin/bash
# Example: Running local tests for mqvpn development

# This file shows common testing workflows for mqvpn developers

# ── Setup (one-time) ────────────────────────────────────────────────────────

# Install build dependencies
sudo apt-get update
sudo apt-get install -y cmake ninja-build pkg-config gcc g++ make \
    libssl-dev libevent-dev iproute2 iptables netcat-openbsd openssl

# Clone repository
git clone https://github.com/getlantern/mqvpn.git
cd mqvpn
git submodule update --init --recursive

# ── Development Workflow ────────────────────────────────────────────────────

# 1. Make your code changes
# vim src/your_changes.c

# 2. Validate locally before pushing
# Option A: Quick build-only check (30-60s)
sudo ./scripts/run_all_local_tests.sh --build-only

# Option B: Full test suite (2-3 min)
sudo ./scripts/run_all_local_tests.sh

# Option C: Only run tests (assumes build exists)
sudo ./scripts/run_all_local_tests.sh --tests-only

# ── Testing Your Changes ────────────────────────────────────────────────────

# Test specific functionality:

# 1. Testing path management changes?
#    Run unit tests for pending path regression first
./build/tests/test_server | grep pending

# 2. Testing control API changes?
#    Check get_status and list_paths tests
./build/tests/test_server | grep get_status

#    Run E2E to verify full integration
sudo ./scripts/ci_e2e/run_path_api_test.sh ./build/mqvpn

# 3. Testing server-side changes?
#    Focus on unit tests (no namespace needed)
./build/tests/test_server

# 4. Testing client-side changes?
#    Run full E2E suite with namespaces
sudo ./scripts/run_all_local_tests.sh

# ── Debugging Failed Tests ──────────────────────────────────────────────────

# If a test fails, check logs:

# 1. Unit test failed? Run with verbose output
./build/tests/test_server 2>&1 | tail -50

# 2. E2E test failed? Check namespace test logs
# The E2E script creates temporary logs in /tmp/tmp.*/
ls -la /tmp/tmp.*/client.log /tmp/tmp.*/server.log 2>/dev/null

# 3. Look at detailed error from failed E2E test
sudo ./scripts/ci_e2e/run_path_api_test.sh ./build/mqvpn 2>&1

# ── Continuous Testing During Development ──────────────────────────────────

# Watch mode: rerun tests on file changes (requires entr)
# sudo apt-get install entr
#
# find src/ tests/ -name "*.c" -o -name "*.h" | \
#   entr -r bash -c 'sudo ./scripts/run_all_local_tests.sh --tests-only'

# ── Performance Testing ─────────────────────────────────────────────────────

# Measure test runtime
time sudo ./scripts/run_all_local_tests.sh

# Profile build time
cmake --build build --verbose 2>&1 | \
  grep "real\|user\|sys" 

# ── Pre-commit Hook Setup ──────────────────────────────────────────────────

# Create a pre-commit hook to run tests automatically
cat > .git/hooks/pre-commit << 'EOF'
#!/bin/bash
echo "Running local tests before commit..."
sudo ./scripts/run_all_local_tests.sh --tests-only || {
    echo "Tests failed! Commit aborted."
    exit 1
}
EOF
chmod +x .git/hooks/pre-commit

# ── Cleanup ─────────────────────────────────────────────────────────────────

# Clean build artifacts
rm -rf build/

# Clean test artifacts
rm -rf .test-work/

# Force clean rebuild (includes third-party)
git clean -fdX
git submodule foreach git clean -fdX
mkdir build && cd build && cmake .. && ninja

# ── Integration with GitHub Actions ─────────────────────────────────────────

# These local tests mirror the GitHub Actions workflows:
#
# test-path-api.yml runs:
#   ✓ unit-tests job     → ./build/tests/test_server
#   ✓ path-api-e2e job   → ./scripts/ci_e2e/run_path_api_test.sh
#
# test-pending-regression.yml runs:
#   ✓ test_server target → ./build/tests/test_server (subset)
#
# To verify your changes will pass CI:
#   sudo ./scripts/run_all_local_tests.sh

# ── Troubleshooting Tips ────────────────────────────────────────────────────

# Build hangs?
#   → Kill previous cmake processes: pkill -9 cmake
#   → Check disk space: df -h
#   → Try without parallel jobs: CMAKE_BUILD_TYPE=Debug make -j1

# Namespace tests fail?
#   → Ensure running as root: whoami  (should print "root")
#   → Check network namespace support: ip netns list
#   → Clean up stray namespaces: sudo ip netns delete vpn-* 2>/dev/null

# Tests pass locally but fail in CI?
#   → Check environment differences: uname -a, gcc --version
#   → Run tests multiple times (race conditions): for i in {1..5}; do ... done
#   → Check system resource limits: ulimit -a

# ── Test Result Examples ────────────────────────────────────────────────────

# Successful run shows:
# ================================================================
#   = TEST RESULTS SUMMARY
# ================================================================
# 
# [PASS]: Unit test: server_get_status_no_clients
# [PASS]: Unit test: server_get_status_with_client
# [PASS]: Unit test: server_runtime_added_path_not_stuck_pending
# [PASS]: All 25 unit tests passed
# [PASS]: E2E test: Path API (add/remove/list_paths/get_status)
# ...
# ✓ All test suites PASSED

# Failed run shows:
# ================================================================
#   = TEST RESULTS SUMMARY  
# ================================================================
#
# [PASS]: Unit test: server_get_status_no_clients
# [FAIL]: Unit test: server_get_status_with_client
# ...
# ✗ Some test suites FAILED
