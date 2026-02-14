#!/usr/bin/env bash
# Deploy and run KVS Master (connects to public Relay).
# Usage: export KVS_RELAY_URL=your-server.com:4433 && ./scripts/deploy-kvs-master.sh [channel]
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${REPO_DIR:-$(cd "$SCRIPT_DIR/.." && pwd)}"
cd "$ROOT"
CHANNEL="${1:-kvs-relay-channel}"
[ -z "${KVS_RELAY_URL}" ] && echo "Set KVS_RELAY_URL (e.g. export KVS_RELAY_URL=host:4433)" && exit 1
echo "=== KVS Master ==="
BUILD_DIR="$ROOT/build-relay"
mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"
[ ! -f Makefile ] && cmake .. -DUSE_OPENSSL=ON -DUSE_MBEDTLS=OFF -DBUILD_SAMPLE=ON
make -j"$(nproc 2>/dev/null || echo 4)"
for d in "$ROOT/open-source/lib64" "$ROOT/open-source/lib"; do [ -d "$d" ] && export LD_LIBRARY_PATH="$d${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"; done
[ ! -x "$BUILD_DIR/samples/kvsWebrtcClientMaster" ] && echo "Build failed" && exit 1
cd "$BUILD_DIR" && exec ./samples/kvsWebrtcClientMaster "$CHANNEL"
