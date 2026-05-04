# Local Testing Guide for mqvpn

This guide explains how to run all mqvpn tests locally using the comprehensive test runner.

## Quick Start

### Prerequisites
```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install -y \
    cmake ninja-build pkg-config \
    gcc g++ make \
    libssl-dev libevent-dev \
    iproute2 iptables netcat-openbsd openssl
```

### Run All Tests
```bash
cd /path/to/mqvpn
sudo ./scripts/run_all_local_tests.sh
```

## Test Coverage

The local test suite includes:

### Unit Tests (Loopback)
- **Control API Tests** (get_status)
  - `server_get_status_no_clients` - Verify get_status returns empty list with no connected clients
  - `server_get_status_with_client` - Verify get_status returns correct client info
  
- **Pending Path Regression Tests** (#4273)
  - `server_runtime_added_path_not_stuck_pending` - Verify runtime-added paths don't stay PENDING indefinitely

### E2E Tests (Network Namespaces)
- **Path API Operations**
  - Phase 1: Single-path tunnel establishment on path A
  - Phase 2: Runtime path addition (Bug 1 + #4273 verification)
  - Phase 3: Path removal and tunnel survival on secondary paths (Bug 2)
  - Phase 4: Control API commands (list_paths, get_status)

## Test Script Options

```bash
# Build and run all tests
sudo ./scripts/run_all_local_tests.sh

# Only build (skip tests)
sudo ./scripts/run_all_local_tests.sh --build-only

# Only run tests (skip build if already present)
sudo ./scripts/run_all_local_tests.sh --tests-only

# Show help
sudo ./scripts/run_all_local_tests.sh --help
```

## Individual Test Execution

### Unit Tests Only
```bash
./build/tests/test_server
```

Output should show:
```
   25/25 tests passed
```

### E2E Path API Tests Only
```bash
sudo ./scripts/ci_e2e/run_path_api_test.sh ./build/mqvpn
```

Output should show:
```
   All tests PASSED
   Phase 1: single-path tunnel established on path A
   Phase 2: paths B/C activated via API (Bug 1 + #4273 verified)
   Phase 3: tunnel survives on secondary paths after removing path A (Bug 2)
   Phase 4: list_paths and get_status control API commands validated
```

## Test Architecture

### Unit Tests
- Location: `tests/test_server.c`
- Infrastructure: Loopback QUIC integration tests
- Setup: Creates local QUIC client/server on localhost
- Cleanup: Automatic on exit
- Dependencies: libmqvpn, xquic, BoringSSL

### E2E Tests
- Location: `scripts/ci_e2e/run_path_api_test.sh`
- Infrastructure: Network namespaces (Linux)
- Setup: Creates isolated namespaces with veth pairs
- Cleanup: Automatic on exit (trap)
- Dependencies: iproute2, netcat, root privileges
- Topology: Triple-path VPN tunnel (A, B, C) over virtual ethernet

### Master Test Runner
- Location: `scripts/run_all_local_tests.sh`
- Purpose: Orchestrates all tests with unified reporting
- Output: Color-coded results with summary
- Exit Code: 0 (all pass) or 1 (any fail)

## Troubleshooting

### Build Failures

#### cmake not found
```bash
sudo apt-get install cmake ninja-build
```

#### SSL/TLS errors
```bash
sudo apt-get install libssl-dev
```

#### Missing third-party dependencies
```bash
git submodule update --init --recursive
```

### Runtime Failures

#### Permission denied (network namespaces)
The script requires root for namespace operations:
```bash
sudo ./scripts/run_all_local_tests.sh
```

#### Namespace already exists
The script cleans up previous test namespaces. If they persist:
```bash
sudo ip netns delete vpn-server-pa 2>/dev/null || true
sudo ip netns delete vpn-client-pa 2>/dev/null || true
```

#### Tests hang or timeout
- Increase system resources: `ulimit -n 4096`
- Check firewall isn't blocking loopback: `sudo iptables -L -n | grep -i lo`
- Verify networking: `ping -c 1 127.0.0.1`

## Continuous Integration

These same tests are run in GitHub Actions workflows:
- `.github/workflows/test-path-api.yml` - Unit tests + E2E tests
- `.github/workflows/test-pending-regression.yml` - Pending path regression

The local test runner can be used for pre-commit validation:
```bash
#!/bin/bash
# .git/hooks/pre-commit
sudo ./scripts/run_all_local_tests.sh --tests-only || exit 1
```

## Performance

Expected runtime (on moderate hardware):
- Build: 30-60 seconds (BoringSSL + xquic + mqvpn)
- Unit tests: 3-5 seconds
- E2E tests: 15-30 seconds
- **Total: ~2-3 minutes**

## Environment Variables

The local test runner respects these environment variables:

```bash
# Build type (default: Release)
CMAKE_BUILD_TYPE=Debug sudo ./scripts/run_all_local_tests.sh

# Parallel build jobs (default: nproc)
JOBS=4 sudo ./scripts/run_all_local_tests.sh

# Custom work directory (default: .test-work)
WORK_DIR=/tmp/mqvpn-tests sudo ./scripts/run_all_local_tests.sh
```

## Contributing

When adding new tests:

1. **Unit tests**: Add to `tests/test_server.c` (loopback integration)
2. **E2E tests**: Add phases to `scripts/ci_e2e/run_path_api_test.sh`
3. **Runner updates**: Update `scripts/run_all_local_tests.sh` to report new tests
4. **CI updates**: Ensure new tests are covered in `.github/workflows/`

Test your changes locally first:
```bash
sudo ./scripts/run_all_local_tests.sh
```
