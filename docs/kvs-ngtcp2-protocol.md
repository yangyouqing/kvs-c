# kvs-ngtcp2 Protocol Specification

This document defines the application protocol for kvs-ngtcp2: signaling and media over a single QUIC connection to a Relay server, with "connect first, optimize later" (先通后优) semantics.

## 1. Overview

- **Clients**: KVS Master and KVS Viewer.
- **Relay**: A single ngtcp2-based QUIC server. Clients establish one QUIC connection each to the Relay; signaling and (during 先通) media are multiplexed over streams on that connection.
- **先通**: Signaling and media flow through the Relay. No inbound hole-punching required; works through symmetric NAT.
- **后优**: Relay provides coturn STUN/TURN in JoinChannel response; clients may attempt WebRTC P2P (ICE). On success, media can switch from QUIC relay to WebRTC direct.

## 2. QUIC Connection and Stream Usage

All stream IDs are fixed. Client-initiated streams have even IDs (0, 2, 4, ...); server-initiated (if any) odd.

| Stream ID | Direction | Purpose |
|-----------|-----------|---------|
| **0** | Bidirectional (client opens) | Signaling. JSON text messages; see §3 and §4. |
| **1** | Client → Server (client opens) | Audio media (先通 phase). Binary: length-prefixed frames. |
| **2** | Client → Server (client opens) | Video media (先通 phase). Binary: length-prefixed frames. |

- Relay binds each client connection to a channel and clientId after JoinChannel. It forwards:
  - **Signaling**: from one peer's stream 0 to the other peer's stream 0 (by RecipientClientId).
  - **Media**: from peer A's stream 1/2 to peer B's stream 1/2 (and vice versa for viewer→master if needed).

## 3. Signaling Stream (Stream 0)

### 3.1 Message Framing

- Each signaling message is a **single JSON object**, UTF-8 encoded.
- Implementation may send one JSON object per QUIC stream write, or use a delimiter (e.g. newline) or length prefix. This spec assumes **one JSON object per write**; the receiver parses until a complete JSON object is available.

### 3.2 Application Signaling (KVS-Compatible)

After JoinChannel has completed, all application signaling on stream 0 uses the same JSON shape as KVS WebSocket signaling, so existing KVS C code (`parseSignalingMessage`, `sendLwsMessage` templates) can be reused.

**Outgoing (client sends to Relay; Relay forwards to peer):**

- `action`: string — one of `SDP_OFFER`, `SDP_ANSWER`, `ICE_CANDIDATE`, `GO_AWAY`, `RECONNECT_ICE_SERVER`, `STATUS_RESPONSE`.
- `RecipientClientId`: string — target client id.
- `MessagePayload`: string — Base64-encoded payload (SDP or ICE candidate JSON).
- `CorrelationId`: string (optional).

**Incoming (Relay delivers to client; parsed as ReceivedSignalingMessage):**

- `senderClientId`: string.
- `messageType`: same as `action` above.
- `messagePayload`: Base64-decoded by receiver; same semantics as KVS.
- `correlationId`, `statusCode`, `errorType`, `description` for STATUS_RESPONSE.

Relay does not modify application signaling; it only routes by RecipientClientId.

### 3.3 JoinChannel (First Message on Stream 0)

Before any application signaling, the client **must** send a JoinChannel request. No other client in the channel will receive this; only the Relay processes it.

**JoinChannel Request (client → Relay):**

```json
{
  "messageType": "JOIN_CHANNEL",
  "channelName": "<string, max 256>",
  "clientId": "<string, max 256>",
  "role": "master" | "viewer",
  "token": "<optional string>"
}
```

**JoinChannel Response (Relay → client):**

- **Success (200):**

```json
{
  "messageType": "JOIN_CHANNEL_RESPONSE",
  "statusCode": 200,
  "iceServers": [
    {
      "ttl": <number, seconds>,
      "uris": ["stun:host:port", "turn:host:port?transport=udp"],
      "username": "<string>",
      "password": "<string>"
    }
  ],
  "reflexiveAddress": "<optional string, e.g. ip:port for 后优>"
}
```

- **Error (4xx/5xx):**

```json
{
  "messageType": "JOIN_CHANNEL_RESPONSE",
  "statusCode": <number>,
  "errorType": "<string>",
  "description": "<string>"
}
```

- `iceServers` aligns with KVS `IceConfigInfo`: each entry has `ttl`, `uris[]`, `username`, `password`. TTL is in seconds. Relay fills this from configured coturn (or static config for testing).
- After success, the client is "joined"; subsequent messages on stream 0 are application signaling and are forwarded by RecipientClientId.

## 4. Media Streams (Streams 1 and 2)

### 4.1 Frame Format (先通 Phase)

- **Binary**: each write is **length-prefixed** then raw payload.
  - Length: 4 bytes, big-endian, unsigned (max 4GB). Length is the number of bytes of the payload that immediately follow.
  - Payload: encoded audio (e.g. Opus) or video (e.g. H264/H265) frame, or RTP-like packet, as agreed by implementation.
- No additional RTP header is required if the implementation sends raw codec frames; if RTP is used over QUIC, the payload is the full RTP packet.

### 4.2 Stream Roles

- **Master**: typically sends on stream 1 (audio) and stream 2 (video). May receive on 1/2 if viewer sends (e.g. back-channel).
- **Viewer**: typically receives on stream 1 and 2; may send on 1/2 if needed.
- Relay forwards stream 1 data from one peer to the other’s stream 1, and stream 2 to stream 2.

## 5. 后优 (Optimize: Try Direct)

- **ICE config**: Already provided in JoinChannel response (`iceServers`). Clients use it to create a PeerConnection and run ICE (STUN/TURN via coturn).
- **Reflexive address**: Optional in JoinChannel response; Relay can include the client’s reflexive (e.g. from first packet) for the other peer to try simultaneous open or ICE.
- **Application signaling**: Offer/Answer and ICE_CANDIDATE are exchanged over stream 0 (unchanged). No new message types are required for basic 后优.
- **Optional control messages** (can be added later):
  - `TryDirect`: Relay tells both peers to attempt direct connection.
  - `ReflexiveAddress`: Relay sends the other peer’s reflexive to each client.
  - `SwitchToDirect`: Client notifies that media has switched to WebRTC; Relay may stop forwarding media on streams 1/2 for that session.

## 6. Constants (Align with KVS)

- Channel name length: 256.
- Client id length: 256.
- Signaling message payload max length: 18750.
- ICE: `MAX_ICE_CONFIG_COUNT` (5), `MAX_ICE_CONFIG_URI_COUNT` (4), URI length 127, username/credential length 256.

## 7. Connection Lifecycle

1. Client resolves Relay host/port (e.g. 443), establishes QUIC connection (ngtcp2 client).
2. Client opens stream 0, sends JoinChannel request.
3. Relay responds with JoinChannel response (200 + iceServers, or error).
4. On 200, client opens streams 1 and 2 if it will send media; Relay binds connection to channel and clientId.
5. Application signaling (Offer/Answer/ICE_CANDIDATE) flows on stream 0; Relay forwards by RecipientClientId.
6. Media (先通) flows on streams 1 and 2; Relay forwards to the other peer.
7. For 后优: client creates PeerConnection with iceServers from JoinChannel, exchanges SDP/ICE over stream 0; when ICE reaches connected and path is direct, client may switch media to WebRTC and optionally stop using streams 1/2.
