#!/usr/bin/env bash
# Build a Debian package for mqvpn.
# Usage: scripts/build_deb.sh [--install-deps] [dpkg-buildpackage options]
#
# Pass --install-deps to install build dependencies via apt before building.

set -euo pipefail

cd "$(dirname "$0")/.."

INSTALL_DEPS=0
DPB_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --install-deps) INSTALL_DEPS=1 ;;
        *)              DPB_ARGS+=("$arg") ;;
    esac
done

if [ $INSTALL_DEPS -eq 1 ]; then
    echo "==> Installing build dependencies..."
    # mk-build-deps installs a dummy package that depends on Build-Depends
    if command -v mk-build-deps &>/dev/null; then
        sudo mk-build-deps --install --remove debian/control \
            --tool 'apt-get -y --no-install-recommends'
    else
        sudo apt-get install -y --no-install-recommends \
            debhelper-compat devscripts dpkg-dev \
            cmake libevent-dev libssl-dev git \
            golang-go ninja-build perl
    fi
fi

# Ensure submodules are present
if [ ! -f third_party/xquic/CMakeLists.txt ]; then
    echo "==> Initialising submodules..."
    git submodule update --init --recursive
fi

echo "==> Building package (this will compile BoringSSL + xquic + mqvpn)..."
dpkg-buildpackage -us -uc -b "${DPB_ARGS[@]}"

echo ""
echo "==> Done. Package(s) written to parent directory:"
ls -1 ../mqvpn_*.deb ../libmqvpn*.deb 2>/dev/null || true
