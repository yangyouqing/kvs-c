#!/usr/bin/env bash
# 无 Docker 本机一键跑通：coturn + relay + Master + Viewer
# 用法: ./scripts/run-relay-local-no-docker.sh [channel-name]

set -e
CHANNEL="${1:-kvs-relay-channel}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

RELAY_PID=""
MASTER_PID=""
COTURN_PID=""
cleanup() {
  echo ""
  echo "正在结束 Master、Relay..."
  [ -n "$MASTER_PID" ] && kill "$MASTER_PID" 2>/dev/null || true
  [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null || true
  [ -n "$COTURN_PID" ] && kill "$COTURN_PID" 2>/dev/null || true
  exit 0
}
trap cleanup INT TERM

echo "=== 1. 证书 certs/server.pem, certs/server.key ==="
if [ ! -f certs/server.pem ] || [ ! -f certs/server.key ]; then
  echo "生成自签名证书..."
  mkdir -p certs
  openssl req -x509 -newkey rsa:2048 -keyout certs/server.key -out certs/server.pem -days 365 -nodes -subj "/CN=localhost"
fi

echo "=== 2. Coturn (端口 3478) ==="
if command -v ss &>/dev/null; then
  LISTEN_CHECK="ss -ulnp"
elif command -v netstat &>/dev/null; then
  LISTEN_CHECK="netstat -ulnp"
else
  LISTEN_CHECK=""
fi
if [ -n "$LISTEN_CHECK" ] && $LISTEN_CHECK 2>/dev/null | grep -q ":3478"; then
  echo "Coturn 已在 3478 监听，跳过启动"
elif command -v turnserver &>/dev/null; then
  echo "启动 turnserver (后台)..."
  turnserver -p 3478 --relay-ip=127.0.0.1 --external-ip=127.0.0.1 --user=user:pass --realm=local --no-cli &
  COTURN_PID=$!
  sleep 2
  if [ -n "$LISTEN_CHECK" ] && ! $LISTEN_CHECK 2>/dev/null | grep -q ":3478"; then
    echo "若 3478 未监听，请手动启动 coturn: sudo systemctl start coturn 或 sudo turnserver ..."
  fi
else
  echo "未找到 turnserver，请先安装 coturn 并启动（如 sudo systemctl start coturn）"
  exit 1
fi

echo "=== 3. 编译并启动 Relay (4433) ==="
RELAY_BUILD="$ROOT/kvs-ngtcp2-server/build"
mkdir -p "$RELAY_BUILD"
cd "$RELAY_BUILD"
[ ! -f Makefile ] && cmake .. && make
cd "$ROOT"
"$RELAY_BUILD/kvs-ngtcp2-server" --bind 0.0.0.0 --port 4433 \
  --cert "$ROOT/certs/server.pem" --key "$ROOT/certs/server.key" \
  --stun "stun:127.0.0.1:3478" --turn "turn:127.0.0.1:3478?transport=udp" \
  --turn-user user --turn-pass pass &
RELAY_PID=$!
sleep 2
if ! kill -0 $RELAY_PID 2>/dev/null; then
  echo "Relay 启动失败"
  exit 1
fi

echo "=== 4. 编译 KVS 客户端 (USE_OPENSSL) ==="
BUILD_DIR="$ROOT/build-relay"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ ! -f Makefile ]; then
  cmake .. -DUSE_OPENSSL=ON -DUSE_MBEDTLS=OFF -DBUILD_SAMPLE=ON
fi
make -j"$(nproc 2>/dev/null || echo 4)"

echo "=== 5. Master (后台) + Viewer (前台) ==="
export KVS_RELAY_URL="127.0.0.1:4433"
# 运行时加载 open-source 下的 so（如 libmbedcrypto、libssl 等）
for d in "$ROOT/open-source/lib64" "$ROOT/open-source/lib"; do
  [ -d "$d" ] && export LD_LIBRARY_PATH="$d${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
done
MASTER_EXE="$BUILD_DIR/samples/kvsWebrtcClientMaster"
VIEWER_EXE="$BUILD_DIR/samples/kvsWebrtcClientViewer"
[ ! -x "$MASTER_EXE" ] && echo "不存在: $MASTER_EXE" && exit 1
[ ! -x "$VIEWER_EXE" ] && echo "不存在: $VIEWER_EXE" && exit 1

cd "$BUILD_DIR"
echo "启动 Master (channel=$CHANNEL)..."
"$MASTER_EXE" "$CHANNEL" &
MASTER_PID=$!
sleep 4
echo "启动 Viewer (channel=$CHANNEL)，Ctrl+C 结束"
"$VIEWER_EXE" "$CHANNEL"
cleanup
