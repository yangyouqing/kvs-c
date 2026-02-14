#!/usr/bin/env bash
# 在公网服务器上部署 kvs-ngtcp2-server (Relay)
# 用法: 将本仓库拷到目标服务器后，在项目根目录执行
#   export TURN_HOST=本机或TURN服务器IP   # Relay 在 Join 响应里下发的 STUN/TURN 地址
#   export TURN_PORT=3478
#   ./scripts/deploy-kvs-ngtcp2-server.sh
#
# 环境变量:
#   TURN_HOST     - TURN 服务器地址（IP 或域名），Relay 用其拼 stun/turn URI 下发给客户端
#   TURN_PORT     - TURN 端口，默认 3478
#   TURN_USER     - TURN 用户名，默认 user
#   TURN_PASS     - TURN 密码，默认 pass
#   RELAY_BIND    - Relay 监听地址，默认 0.0.0.0
#   RELAY_PORT    - Relay 监听端口，默认 4433
#   CERT_FILE     - TLS 证书路径，默认 certs/server.pem
#   KEY_FILE      - TLS 私钥路径，默认 certs/server.key
#   REPO_DIR      - kvs-c 仓库根目录，默认脚本所在目录的上一级

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${REPO_DIR:-$(cd "$SCRIPT_DIR/.." && pwd)}"
cd "$ROOT"

TURN_PORT="${TURN_PORT:-3478}"
TURN_USER="${TURN_USER:-user}"
TURN_PASS="${TURN_PASS:-pass}"
RELAY_BIND="${RELAY_BIND:-0.0.0.0}"
RELAY_PORT="${RELAY_PORT:-4433}"
CERT_FILE="${CERT_FILE:-$ROOT/certs/server.pem}"
KEY_FILE="${KEY_FILE:-$ROOT/certs/server.key}"

if [ -z "${TURN_HOST}" ]; then
  echo "错误: 请设置 TURN 服务器地址，例如: export TURN_HOST=你的公网IP或域名"
  exit 1
fi

echo "=== 部署 kvs-ngtcp2-server (Relay) 到公网服务器 ==="
echo "  TURN_HOST=$TURN_HOST TURN_PORT=$TURN_PORT"
echo "  RELAY_BIND=$RELAY_BIND RELAY_PORT=$RELAY_PORT"
echo "  CERT=$CERT_FILE KEY=$KEY_FILE"

# 证书
if [ ! -f "$CERT_FILE" ] || [ ! -f "$KEY_FILE" ]; then
  echo "生成自签名证书到 certs/..."
  mkdir -p "$ROOT/certs"
  openssl req -x509 -newkey rsa:2048 -keyout "$ROOT/certs/server.key" -out "$ROOT/certs/server.pem" -days 365 -nodes -subj "/CN=$TURN_HOST"
  CERT_FILE="$ROOT/certs/server.pem"
  KEY_FILE="$ROOT/certs/server.key"
fi

# 编译
RELAY_BUILD="$ROOT/kvs-ngtcp2-server/build"
mkdir -p "$RELAY_BUILD"
cd "$RELAY_BUILD"
if [ ! -f Makefile ]; then
  cmake .. && make
else
  make -j"$(nproc 2>/dev/null || echo 2)"
fi
cd "$ROOT"

STUN_URI="stun:${TURN_HOST}:${TURN_PORT}"
TURN_URI="turn:${TURN_HOST}:${TURN_PORT}?transport=udp"

echo "启动 Relay（前台，Ctrl+C 结束）..."
exec "$RELAY_BUILD/kvs-ngtcp2-server" \
  --bind "$RELAY_BIND" --port "$RELAY_PORT" \
  --cert "$CERT_FILE" --key "$KEY_FILE" \
  --stun "$STUN_URI" --turn "$TURN_URI" \
  --turn-user "$TURN_USER" --turn-pass "$TURN_PASS"
