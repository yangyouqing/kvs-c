# kvs-ngtcp2 部署（无 Docker，本机）

本机跑通四件套：**coturn**、**kvs-ngtcp2-server (relay)**、**Master**、**Viewer**，全部原生运行，无需 Docker。

## 前置

- OpenSSL、CMake、GCC
- coturn（STUN/TURN）

## 一键脚本（无 Docker）

```bash
./scripts/run-relay-local-no-docker.sh [channel-name]
```

默认 channel：`kvs-relay-channel`。脚本会：

1. 检查/生成证书 `certs/server.pem`、`certs/server.key`
2. 若未检测到 coturn（端口 3478），尝试启动 `turnserver`（需已安装 coturn）
3. 编译并**后台**启动 relay（4433）
4. 用 **USE_OPENSSL** 编译 KVS 客户端
5. **后台**启动 Master，**前台**启动 Viewer

按 **Ctrl+C** 会结束 Viewer，并自动停掉 Master 和 Relay。

**需先安装 coturn**（若未装）：

```bash
# Ubuntu/Debian
sudo apt-get update && sudo apt-get install -y coturn
```

---

## 手动步骤（无 Docker）

### 1. 证书

无则生成（项目根目录下）：

```bash
mkdir -p certs
openssl req -x509 -newkey rsa:2048 -keyout certs/server.key -out certs/server.pem -days 365 -nodes -subj "/CN=localhost"
```

### 2. 启动 Coturn

**方式 A：systemd（推荐）**

```bash
sudo apt-get install -y coturn
sudo sed -i 's/#TURNSERVER_ENABLED=1/TURNSERVER_ENABLED=1/' /etc/default/coturn
# 本机测试可用静态用户
echo 'user=user:pass' | sudo tee /etc/turnserver.conf.d/user.conf
sudo systemctl start coturn
```

**方式 B：前台运行（调试用）**

```bash
turnserver --listening-port=3478 --relay-ip=127.0.0.1 --external-ip=127.0.0.1 --user=user:pass --realm=local --no-cli
```

### 3. 编译并启动 Relay

终端 1：

```bash
cd kvs-ngtcp2-server
mkdir -p build && cd build
cmake .. && make
./kvs-ngtcp2-server --bind 0.0.0.0 --port 4433 \
  --cert ../../certs/server.pem --key ../../certs/server.key \
  --stun "stun:127.0.0.1:3478" --turn "turn:127.0.0.1:3478?transport=udp" \
  --turn-user user --turn-pass pass
```

### 4. 编译 KVS 客户端（OpenSSL）

终端 2：

```bash
cd /path/to/kvs-c
mkdir -p build-relay && cd build-relay
cmake .. -DUSE_OPENSSL=ON -DUSE_MBEDTLS=OFF -DBUILD_SAMPLE=ON
make -j$(nproc)
```

### 5. 启动 Master 与 Viewer

依赖库（如 libmbedcrypto、libssl）在 `open-source/lib` 或 `open-source/lib64`，需设置 `LD_LIBRARY_PATH` 再运行：

终端 3（Master）：

```bash
export KVS_RELAY_URL=127.0.0.1:4433
export LD_LIBRARY_PATH="/path/to/kvs-c/open-source/lib64:/path/to/kvs-c/open-source/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd /path/to/kvs-c/build-relay
./samples/kvsWebrtcClientMaster kvs-relay-channel
```

终端 4（Viewer）：

```bash
export KVS_RELAY_URL=127.0.0.1:4433
export LD_LIBRARY_PATH="/path/to/kvs-c/open-source/lib64:/path/to/kvs-c/open-source/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd /path/to/kvs-c/build-relay
./samples/kvsWebrtcClientViewer kvs-relay-channel
```

将 `/path/to/kvs-c` 换成你的项目根目录。Master 与 Viewer 使用**相同 channel 名**即可互通。

---

## 端口

| 服务   | 端口 | 说明                    |
|--------|------|-------------------------|
| Relay  | 4433 | TCP+TLS（信令 + 媒体）  |
| Coturn | 3478 | STUN/TURN (UDP)        |

## 环境变量（KVS 客户端）

- **KVS_RELAY_URL**：设为 `127.0.0.1:4433`（或 `host:4433`）时走 kvs-ngtcp2 relay，必设。

## 分机部署（4 个脚本）

**turnserver** 与 **kvs-ngtcp2-server** 部署到公网服务器；**Master**、**Viewer** 在本地或任意机器运行，通过 `KVS_RELAY_URL` 连接公网 Relay。

### 1. 公网服务器：TURN (coturn)

在公网服务器上执行（需先安装 coturn，见上文）：

```bash
export EXTERNAL_IP=你的公网IP    # 必填，如 1.2.3.4
# 可选: TURN_PORT=3478  TURN_USER=user  TURN_PASS=pass
./scripts/deploy-turnserver.sh
```

脚本会尝试用 systemd 启动 coturn；若无 `/etc/turnserver.conf.d` 或需前台调试，可设 `NO_SYSTEMD=1` 再运行。

### 2. 公网服务器：kvs-ngtcp2-server (Relay)

在同一台或另一台公网服务器上，将本仓库拷到服务器后执行：

```bash
export TURN_HOST=公网IP或域名     # Relay 下发给客户端的 STUN/TURN 地址，必填
# 可选: TURN_PORT=3478  TURN_USER=user  TURN_PASS=pass
# 可选: RELAY_PORT=4433  CERT_FILE=  KEY_FILE=
./scripts/deploy-kvs-ngtcp2-server.sh
```

若 TURN 与 Relay 在同一台机，`TURN_HOST` 填该机公网 IP 或域名即可。脚本会编译 Relay、无证书时生成自签名，并前台运行。

### 3. 本地/客户端：KVS Master

在要推流的机器上（本仓库根目录）：

```bash
export KVS_RELAY_URL=公网Relay地址:4433   # 如 your-server.com:4433
./scripts/deploy-kvs-master.sh [channel-name]
```

默认 channel：`kvs-relay-channel`。脚本会编译 KVS（USE_OPENSSL）并运行 Master。

### 4. 本地/客户端：KVS Viewer

在要拉流的机器上（本仓库根目录）：

```bash
export KVS_RELAY_URL=公网Relay地址:4433
./scripts/deploy-kvs-viewer.sh [channel-name]
```

与 Master 使用**相同 channel 名**即可互通。

### 分机部署环境变量小结

| 脚本 | 必填环境变量 | 可选 |
|------|----------------|------|
| deploy-turnserver.sh | EXTERNAL_IP | TURN_PORT, TURN_USER, TURN_PASS, NO_SYSTEMD |
| deploy-kvs-ngtcp2-server.sh | TURN_HOST | TURN_PORT, TURN_USER, TURN_PASS, RELAY_PORT, CERT_FILE, KEY_FILE, REPO_DIR |
| deploy-kvs-master.sh | KVS_RELAY_URL | channel 参数, REPO_DIR |
| deploy-kvs-viewer.sh | KVS_RELAY_URL | channel 参数, REPO_DIR |

---

## 说明

- Relay 传输仅在 **USE_OPENSSL=ON** 时编译；默认 USE_MBEDTLS 不会包含 relay，需用 OpenSSL 构建后再设 `KVS_RELAY_URL`。
- 生产环境请使用正式 TLS 证书，coturn 建议配公网 IP 与 TURN REST 等鉴权。
