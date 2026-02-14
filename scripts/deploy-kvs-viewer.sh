#!/usr/bin/env bash
# 部署并运行 KVS Viewer（在本地或任意机器，连接公网 Relay）
# 用法: 在 kvs-c 项目根目录执行
#   export KVS_RELAY_URL=公网Relay地址:4433   # 必填
#   ./scripts/deploy-kvs-viewer.sh [channel-name]
#
# 环境变量: KVS_RELAY_URL(必填)  REPO_DIR 为仓库根目录

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${REPO_DIR:-$(cd "$SCRIPT_DIR/.." && pwd)}"
cd "$ROOT"
CHANNEL="${1:-kvs-relay-channel}"

if [ -z "${KVS_RELAY_URL}" ]; then
  echo "错误: 请设置 Relay 地址，例如: export KVS_RELAY_URL=your-server.com:4433"
  exit 1
fi

echo "=== 部署并运行 KVS Viewer ==="
echo "  KVS_RELAY_URL=$KVS_RELAY_URL"
echo "  CHANNEL=$CHANNEL"

BUILD_DIR="$ROOT/build-relay"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ ! -f Makefile ]; then
  cmake .. -DUSE_OPENSSL=ON -DUSE_MBEDTLS=OFF -DBUILD_SAMPLE=ON
fi
make -j"$(nproc 2>/dev/null || echo 4)"

for d in "$ROOT/open-source/lib64" "$ROOT/open-source/lib"; do
  [ -d "$d" ] && export LD_LIBRARY_PATH="$d${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
done

VIEWER_EXE="$BUILD_DIR/samples/kvsWebrtcClientViewer"
[ ! -x "$VIEWER_EXE" ] && echo "不存在: $VIEWER_EXE" && exit 1

cd "$BUILD_DIR"
echo "启动 Viewer (channel=$CHANNEL)，Ctrl+C 结束"
exec "$VIEWER_EXE" "$CHANNEL"
