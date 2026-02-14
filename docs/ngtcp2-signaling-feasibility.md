# 用 ngtcp2 替代 KVS 信令 + coturn 的可行性分析

## 一、目标与范围

- **信令**：用自建 ngtcp2（QUIC）服务替代 AWS KVS 的 WebSocket 信令；Master/Viewer 与信令服务器之间走 QUIC。
- **ICE**：后端部署 coturn 作为 STUN/TURN，由信令服务器下发 coturn 的 ICE 配置。
- **媒体**：保持现有 WebRTC 媒体路径（UDP + DTLS-SRTP），仍通过 ICE 选路，可 P2P 或经 TURN。
- **目标**：P2P 直连率 ≥ 95%。

下文从协议、架构、实现与 P2P 率四方面分析可行性。

---

## 二、当前 KVS 信令在做什么（必须等价实现）

### 2.1 状态机与 API 调用链

当前信令大致流程（`StateMachine.c` + `LwsApiCalls.c`）：

1. **GetToken** — 拿 AWS 凭证（你方方案可改为自建鉴权或固定 token）。
2. **DescribeChannel** — 描述/校验通道（可简化为“通道存在即 OK”或由 ngtcp2 服务维护通道表）。
3. **Create** — 按需创建通道（可合并到“加入通道”一步）。
4. **GetEndpoint** — 返回 WSS 连接 URL（在 QUIC 方案中改为“返回 ngtcp2 服务器地址/端口”或直接由客户端写死）。
5. **GetIceConfig** — **关键**：返回 STUN/TURN URI 及 username/password（必须由 ngtcp2 服务返回 coturn 的配置）。
6. **Connect** — 建立**持久连接**（当前是 WSS），之后所有信令消息在该连接上收发。
7. **Connected 之后**：在**同一连接**上收发 Offer / Answer / ICE_CANDIDATE 等。

也就是说：**ngtcp2 服务需要提供“连接 + 通道/会话 + ICE 配置 + 消息路由”的等价能力**，而不是只做“发一条消息”。

### 2.2 信令消息类型与格式（必须兼容或显式替换）

- **类型**（见 `LwsApiCalls.h` / `getMessageTypeFromString`）：
  - `SDP_OFFER` / `SDP_ANSWER` / `ICE_CANDIDATE`
  - `GO_AWAY` / `RECONNECT_ICE_SERVER` / `STATUS_RESPONSE`
- **发送格式**（当前 WSS 上送的 JSON）：
  - `action`（即 messageType）、`RecipientClientId`、`MessagePayload`（Base64）、可选 `CorrelationId`。
  - Offer 时可在同一条里带 `IceServerList`（TURN 等），当前 KVS 会从 GetIceConfig 拿到并在这里捎带。
- **接收格式**（`parseSignalingMessage`）：
  - `senderClientId`、`messageType`、`messagePayload`（Base64 解码后给上层）。
  - STATUS_RESPONSE 时还有 `correlationId`、`errorType`、`statusCode`、`description`。

**结论**：  
若希望**尽量少改 KVS C 端**，ngtcp2 服务应：
- 在 QUIC 流上收发与当前 **相同或兼容的 JSON 格式**（含 Base64 payload、上述字段名），并保证 `messageType` 与现有枚举一致；  
或  
- 若改用新格式，则需在 C 端增加一层“QUIC 收发的应用协议 ↔ 现有 SignalingMessage/ReceivedSignalingMessage”的适配（解析/组包），工作量会集中在 C 端。

---

## 三、ngtcp2 替代信令的可行性

### 3.1 能力匹配

- **持久连接**：QUIC 单连接多流，完全可以替代“一条 WSS 长连接”。
- **双向消息**：在 QUIC 流上按序收发 JSON（或自定义二进制+长度），等价于 WSS 上的一帧一消息。
- **多客户端/多通道**：服务端用 `(channel_name, client_id)` 做会话管理，把收到的消息按 `RecipientClientId` 转发到对端 QUIC 连接，逻辑与现有 KVS 信令“按 ClientId 路由”一致。
- **ICE 配置**：在“连接建立/加入通道”的响应里直接返回 coturn 的 STUN/TURN URI、username、password、ttl，C 端仍通过 `signalingClientGetIceConfigInfo` / `signalingClientGetIceConfigInfoCount` 使用（由新的信令实现从 ngtcp2 响应里填充 `ChannelInfo` / 内部 ICE 配置）。

因此，**从协议与行为上，用 ngtcp2 完全可行**，只要服务端实现“连接 → 鉴权 → 通道加入 → 下发 ICE 配置 → 转发 Offer/Answer/ICE_CANDIDATE”的完整流程。

### 3.2 与“信令、音视频同一 QUIC 通道”的两种理解

- **理解 A（推荐）**：  
  - **信令**走 ngtcp2（一条 QUIC 连接）；  
  - **音视频**仍走现有 WebRTC（UDP，DTLS-SRTP，ICE 选路）。  
  - 即：只替换“信令通道”，媒体不变。  
  - P2P 率 95% 主要靠 ICE + coturn，与信令是否 QUIC 无关，**该方案与 95% P2P 目标兼容**。

- **理解 B（信令+媒体同一 QUIC + 先通后优）**：  
  - 信令、音频、视频**全部**走同一条 QUIC（多流或单流复用）；**先通过 Relay 打通，再尝试升级为直连**。  
  - 即：媒体也走 QUIC（如 RTP over QUIC 或自定义媒体流），经 **Relay** 转发保证 100% 先通（含对称 NAT）；再在 Relay 协调下做 **hole punching / 直连尝试**，成功则切换到直连路径（后优）。  
  - 见下节「先通后优与 Iroh 思路对 P2P 直连率的帮助」。

**建议**：若目标是“先通再优、对称 NAT 也能通”，应采用 **理解 B** 并引入 Relay + 后优；若仅替换信令且网络以非对称 NAT 为主，可先按 **理解 A** 落地。

---

## 3.3 先通后优 + Iroh 思路：对 P2P 直连率确实有帮助（修订）

此前结论“信令+媒体同一 QUIC 与提高 P2P 率无直接关系”在**仅替换信令、媒体仍走 WebRTC/ICE** 的前提下成立。一旦采用**信令与媒体同一 QUIC 通道 + Relay + 先通后优**（Iroh 思路），对 P2P 直连率会有**直接或间接帮助**，修正如下。

### Iroh 的“先通后优”在做什么

- **先通**：两端都只做**出站**连接到一个 **Relay**（QUIC 或 WebSocket）。Relay 转发信令和媒体。因此**不依赖入站打洞**，对称 NAT、严格防火墙下也能建立会话，**连通率 ≈ 100%**。
- **后优**：在已通过 Relay 连通的前提下，通过 Relay **交换双方的 reflexive 地址**，再在 Relay 协调下做**同时向对端发起连接**（hole punching / 同时打开）。若成功，媒体切到**直连 QUIC**，Relay 不再转发；若失败（如典型对称 NAT），则继续走 Relay。

### 对 P2P 直连率的三类帮助

1. **不因“先失败”而丢掉直连机会**  
   - 传统 ICE：若信令超时、TURN 不可用或两端都是对称 NAT，整场会话可能**直接失败**，根本没有“尝试直连”这一步。  
   - 先通后优：**先**用 Relay 保证会话建立，**再**在已有会话上尝试直连。因此“能尝试直连的会话”的基数变大，不会因为前期信令/ICE 失败而损失本可以直连的 case，**有利于提高“可尝试直连的会话比例”和最终直连成功数**。

2. **Relay 协调的“同时打开”比传统 ICE 更利于打洞**  
   - 传统 ICE：双方各自收集 candidate，通过信令交换后，各自向对方地址发 binding request，**时间上往往不同步**，在部分 NAT 上容易失败。  
   - Iroh 式做法：Relay 已知双方地址，可**协调“在同一时刻”向对端发起连接**（simultaneous open），更符合很多 NAT 的“只认已发出会话的响应”的语义，**在部分 NAT（包括部分对称型）上能打出洞**，从而**提高直连成功率**（分子变大）。

3. **直连率指标的定义与分子、分母**  
   - 若定义：**P2P 直连率 = 使用直连路径的会话数 / 总成功建立会话数**：  
     - 先通后优使**总成功建立会话数**显著增加（含对称 NAT、弱网），分母变大。  
     - 其中一部分会通过后优升级为直连（分子也变大）。  
     - 在“后优”做得好的前提下，**分子增加可以超过仅靠“分母变大”带来的稀释**，整体直连率仍可提升；即便短期直连率略降，**绝对直连会话数**和**用户体验（先能连上）**都明显变好。  
   - 若定义：**在“理论上可直连”的会话中，实际达成直连的比例**：先通后优避免“因信令/ICE 提前失败而没机会尝试”，这一比例也会**提高**。

因此：**信令+媒体走同一 QUIC 通道 + Relay 先通 + 后优（Iroh 思路）对 P2P 直连率有直接或间接帮助**——既通过“先通”扩大可尝试直连的基数，也通过 Relay 协调的同时打开提高打洞成功率，并改善“先连上再优化”的整体体验。

### 和当前 KVS 架构的对应关系

- **信令+媒体同一 QUIC**：Master/Viewer 各与 ngtcp2 **Relay** 建一条 QUIC，信令与媒体流都在该连接上；Relay 按会话转发。  
- **先通**：不依赖 STUN/TURN 入站，对称 NAT 也能通。  
- **后优**：Relay 在会话内协调“交换地址 + 同时发起直连 QUIC”；成功则媒体切到 A↔B 直连（可保留信令仍经 Relay 或也切直连），失败则继续经 Relay 转发。  
- **coturn**：可在“后优”阶段作为 STUN/TURN 辅助（例如提供 reflexive 或作 TURN 中继），与 Relay 先通后优**叠加**使用，不冲突。

---

## 四、实现路径与工作量（按理解 A）

### 4.1 ngtcp2 信令服务端（新实现）

- **监听**：QUIC 服务（ngtcp2 server），例如单端口 UDP。
- **连接建立**：  
  - 客户端连上后发“加入通道”请求（如 channel_name, role=master/viewer, client_id）。  
  - 服务端鉴权（token 或证书），在内存/缓存中维护 channel → list of (client_id, conn/stream)。
- **ICE 配置**：  
  - 在“加入通道”的响应（或单独一个“getIceConfig”请求的响应）中返回固定或动态的 coturn 配置：  
    - STUN URI：`stun:coturn_host:port`  
    - TURN URI：`turn:coturn_host:port?transport=udp`（及可选 tcp），username、password（coturn 的长期或临时凭证）。  
  - 格式需与 KVS C 端期望的 `IceConfigInfo` 一致（uriCount, uris[], userName, password, ttl）。
- **消息转发**：  
  - 客户端在 QUIC 流上发送与当前 **兼容的 JSON**（action, RecipientClientId, MessagePayload, CorrelationId 等）。  
  - 服务端根据 RecipientClientId 找到对端连接，把整条消息（或按现有格式重组）写到对端 QUIC 流。  
  - 对端 C 端用现有 `parseSignalingMessage` 解析，触发 `signalingMessageReceived` → handleOffer/handleAnswer/handleRemoteCandidate。
- **GO_AWAY / RECONNECT_ICE_SERVER**：  
  - 可由服务端在合适时机主动推一条 GO_AWAY 或 RECONNECT_ICE_SERVER；C 端已有处理，只要 messageType 和 payload 兼容即可。

**工作量**：相当于实现一个“带鉴权 + 通道管理 + ICE 下发 + 消息路由”的 QUIC 应用服务，与 ngtcp2 示例/文档结合，**可行且工作量可估**（约 1–2 人月，视鉴权与运维需求而定）。

### 4.2 KVS C 端改造（两种策略）

**策略 1：最小侵入 — 抽象“信令传输层”**

- 当前：`connectSignalingChannelLws`、`sendLwsMessage`、`writeLwsData`、LWS 收包回调里调 `parseSignalingMessage` 再回调应用。
- 做法：  
  - 引入“信令传输”抽象（如 `SignalingTransport`）：`connect`、`send(message)`、`onReceived(callback)`、`disconnect`。  
  - 现有 LWS 实现保留，实现为 `SignalingTransportLws`。  
  - 新增 `SignalingTransportNgtcp2`：内部用 ngtcp2 建连、在 QUIC 流上收发与当前 **相同格式** 的 JSON，收到后调同一 `parseSignalingMessage` + 同一上层回调。  
  - 状态机、GetToken/Describe/Create/GetEndpoint/GetIceConfig 的**语义**需要映射到 ngtcp2 流程：  
    - 要么在 C 端保留“状态机”，但 GetEndpoint/GetIceConfig 改为从 ngtcp2 的“加入通道”响应里取（Endpoint 可退化为“已知 ngtcp2 地址”）；  
    - 要么简化状态机：Connect 直接等于“连 ngtcp2 + 发加入通道 + 收 ICE 配置”，其余状态合并或省略。
- **GetIceConfig**：  
  - 必须仍能向 PeerConnection 提供 ICE 配置。  
  - 建议：在 ngtcp2 “加入通道” 响应里带 ICE 配置，C 端解析后写入当前信令客户端内部结构（与 `getIceConfigLws` 写入的 `iceConfigs[]` 一致），这样 `signalingClientGetIceConfigInfo` 无需改接口，仍由 `initializePeerConnection` 使用。
- **GetToken/Describe/Create/GetEndpoint**：  
  - 若完全去掉 AWS，可改为：  
    - 固定 token 或本地鉴权；  
    - “Describe/Create” 合并为“加入通道”时服务端检查/创建通道；  
    - “GetEndpoint” 变为配置项（ngtcp2 服务器地址），不再请求。

**策略 2：重写信令客户端**

- 不保留 LWS 路径，新建一套“QUIC-only”信令客户端：状态机精简为“连接 ngtcp2 → 加入通道（含鉴权）→ 收 ICE 配置 → 进入 Connected → 收发消息”。  
- 消息格式若与现有 JSON 一致，上层 Sample 和 `handleOffer`/`handleAnswer`/`handleRemoteCandidate` 可尽量复用；若格式不同，需在重写层做转换。

**推荐**：**策略 1**，便于渐进迁移和回退，且 ICE 与 PeerConnection 逻辑零改动。

### 4.3 依赖与构建

- **当前**：信令依赖 `kvsCommonLws`（含 libwebsockets）、AWS 签名与凭证。  
- **改为 ngtcp2 后**：  
  - 信令路径依赖 ngtcp2（及所选 TLS 库，如 BoringSSL/OpenSSL-quictls）。  
  - 可保留 kvsCommonLws 仅给“仍用 AWS 的构建”或逐步移除；若完全去掉 AWS，则不再需要 LWS 和 AWS 签名。  
- **构建**：新增 CMake 选项（如 `USE_NGTCP2_SIGNALING=ON`），在 ON 时编译 `SignalingTransportNgtcp2` 并链接 ngtcp2，状态机里 Connect/GetEndpoint/GetIceConfig 走新分支。

---

## 五、P2P 直连率 ≥ 95% 的可行性

### 5.1 P2P 率由谁决定

- **决定因素**：NAT/防火墙类型、ICE 配置（STUN/TURN 数量与质量）、是否 Trickle ICE、两端网络是否对称型 NAT 等。  
- **仅替换信令时**：信令从 WSS 换成 QUIC、媒体仍 WebRTC/ICE 时，**信令替换本身不改变** UDP 媒体路径和 ICE 行为，因此不直接提高 P2P 率。  
- **信令+媒体同一 QUIC + 先通后优时**：见 **§3.3**——先通保证会话建立、后优在 Relay 协调下尝试直连，**对 P2P 直连率有直接或间接帮助**（更多会话有机会尝试直连、同时打开提高打洞成功率）。

### 5.2 如何达到 95%+ P2P

- **STUN**：coturn 提供 STUN，用于获取 server reflexive 候选，**必须**。  
- **TURN**：coturn 提供 TURN，在无法 P2P 时中继；部署位置尽量靠近用户（同地域/多地域），减少仅因路径不佳而走 TURN 的比例。  
- **ICE 行为**：  
  - 使用 **Trickle ICE**（当前 KVS 已支持），边收集边交换，减少等待时间，有利于在复杂 NAT 下更快选出 pair。  
  - 保持 `iceTransportPolicy = ALL`（同时 host/srflx/relay），避免人为关掉 relay 导致无法穿透。  
- **网络与运维**：  
  - 若目标用户多在对称型 NAT 或企业网后，真实场景下 P2P 率会受限于此，95% 需要在目标环境中实测；  
  - 通过监控“选中的 candidate pair 类型”（host/srflx/relay）统计 P2P 比例，再针对 relay 占比较高的地区/运营商优化 TURN 部署或策略。

**结论**：在“信令用 ngtcp2 + 媒体仍 WebRTC + coturn 做 STUN/TURN”的架构下，**95% P2P 是可追求的目标**，取决于 coturn 部署与真实网络环境，与是否用 QUIC 做信令无直接矛盾。

---

## 六、风险与注意点

| 风险点 | 说明 | 缓解 |
|--------|------|------|
| ngtcp2 与 KVS 的 TLS 栈 | ngtcp2 需 QUIC-compatible TLS（如 BoringSSL、quictls）；KVS 当前可能用 OpenSSL/MbedTLS 做 DTLS。 | 信令用 ngtcp2 自带 TLS；媒体仍用现有 DTLS，两者可不同库，无冲突。 |
| 消息格式兼容 | 若 ngtcp2 侧 JSON 与现有不一致，C 端解析会失败。 | 服务端严格按现有 action/RecipientClientId/MessagePayload（Base64）/senderClientId/messageType 等实现；或 C 端做薄适配层。 |
| 状态机与 GetIceConfig | 当前 GetIceConfig 由 HTTP 在 Connect 前调用；QUIC 方案可在“加入通道”响应里带 ICE。 | C 端在“连接 ngtcp2 并加入通道”成功后，从响应填充 iceConfigs，并驱动状态机进入 READY → CONNECT；Connect 状态视为“已连接（QUIC）”。 |
| 线程与事件循环 | ngtcp2 是回调/非阻塞 I/O，KVS 信令当前可能依赖 LWS 的线程模型。 | 将 ngtcp2 的 `ngtcp2_conn_read_stream`/写流等接入 KVS 的线程/锁/条件变量或现有 event loop，避免跨线程竞态。 |
| coturn 凭证 | TURN 需 username/password，coturn 支持长期或 TURN REST API 动态凭证。 | 若用动态凭证，ngtcp2 服务需向 coturn 或自建鉴权服务请求临时凭证，再在 GetIceConfig 响应里下发给客户端。 |

---

## 七、总结与建议

- **用 ngtcp2 替代 KVS 信令（WebSocket）**：**可行**。需在服务端实现“QUIC 连接 + 通道/会话 + 鉴权 + ICE 配置下发 + Offer/Answer/ICE_CANDIDATE 转发”，并在 C 端增加 QUIC 传输层（或重写信令客户端），保持与现有信令消息格式和 ICE 使用方式兼容。
- **信令与音视频“同一 QUIC 通道”**：若采用 **先通后优 + Iroh 思路**（信令+媒体同一 QUIC、经 Relay 先通、再协调直连），对 **P2P 直连率有直接或间接帮助**（§3.3），并非“与 P2P 率无关”；建议按目标在“仅替换信令”与“信令+媒体同一 QUIC + Relay 后优”之间做选择。
- **coturn 作 STUN/TURN**：与当前方案兼容，由 ngtcp2 信令服务在连接/加入通道时返回 coturn 的 ICE 配置即可；**P2P 率 95%+ 主要依赖 ICE 与 coturn 部署**，与信令是否 QUIC 无关。
- **实施顺序建议**：  
  1）部署 coturn，确认 STUN/TURN 可达；  
  2）实现 ngtcp2 信令服务（含通道管理、ICE 下发、消息转发、与现有 JSON 兼容）；  
  3）在 KVS C 端做“信令传输层”抽象并实现 ngtcp2 分支，状态机与 GetIceConfig 映射到 QUIC 流程；  
  4）联调 Master/Viewer，再在目标网络下统计 P2P 率并优化。

若你希望下一步细化“ngtcp2 服务端与 C 端接口设计”或“状态机如何从 LWS 迁移到 ngtcp2”，可以指定其中一块，我按该方向继续写接口与伪代码级方案。
