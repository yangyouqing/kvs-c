# kvs-ngtcp2 Relay Server

Implements the kvs-ngtcp2 protocol (see [../docs/kvs-ngtcp2-protocol.md](../docs/kvs-ngtcp2-protocol.md)): clients connect over TLS, send JoinChannel on stream 0, then signaling and media (streams 1 and 2) are forwarded between peers in the same channel.

This build uses **TCP + TLS** with multiplexed streams (frame header: 1 byte stream_id + 4 byte length + payload). The same protocol can be run over ngtcp2 QUIC by replacing the transport layer.

## Build

```bash
mkdir build && cd build
cmake ..
make
```

Requires OpenSSL.

## Run

```bash
./kvs-ngtcp2-server [--bind 0.0.0.0] [--port 4433] [--cert cert.pem] [--key key.pem] \
  [--stun stun:host:port] [--turn turn:host:port?transport=udp] [--turn-user U] [--turn-pass P]
```

Or set env: `KVS_STUN_URI`, `KVS_TURN_URI`, `KVS_TURN_USER`, `KVS_TURN_PASS`.

Generate a test cert:

```bash
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj /CN=localhost
```

## Protocol (stream 0)

1. Client sends one JSON message: `JOIN_CHANNEL` with `channelName`, `clientId`, `role` (master/viewer).
2. Server responds with `JOIN_CHANNEL_RESPONSE` and `iceServers` (coturn STUN/TURN).
3. Further messages on stream 0 are application signaling (action, RecipientClientId, MessagePayload); server forwards by RecipientClientId to the peer in the same channel.
4. Streams 1 and 2: binary length-prefixed frames; server forwards to peer's same stream.
