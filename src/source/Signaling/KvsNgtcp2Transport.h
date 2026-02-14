#ifndef KVS_NGTCP2_TRANSPORT_H
#define KVS_NGTCP2_TRANSPORT_H

#include "../Include_i.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque transport for kvs-ngtcp2 relay (TCP+TLS or QUIC). */
typedef struct KvsNgtcp2Transport KvsNgtcp2Transport;
typedef KvsNgtcp2Transport* PKvsNgtcp2Transport;

/* Create transport. pSignalingClient must outlive the transport. */
STATUS kvsNgtcp2TransportCreate(PSignalingClient pSignalingClient, PKvsNgtcp2Transport* ppTransport);

void kvsNgtcp2TransportDestroy(PKvsNgtcp2Transport* ppTransport);

/* Connect to relay, send JoinChannel, wait response, fill iceConfigs. */
STATUS kvsNgtcp2TransportConnect(PKvsNgtcp2Transport pTransport, PCHAR host, PCHAR port);

STATUS kvsNgtcp2TransportSendMessage(PKvsNgtcp2Transport pTransport, SIGNALING_MESSAGE_TYPE messageType,
    PCHAR peerClientId, PCHAR pPayload, UINT32 payloadLen, PCHAR pCorrelationId, UINT32 correlationIdLen);

void kvsNgtcp2TransportDisconnect(PKvsNgtcp2Transport pTransport);

/* Send a media frame on relay stream 1 (audio) or 2 (video). */
STATUS kvsNgtcp2TransportSendMediaFrame(PKvsNgtcp2Transport pTransport, BOOL isVideo, PBYTE pData, UINT32 dataLen);

#ifdef __cplusplus
}
#endif

#endif /* KVS_NGTCP2_TRANSPORT_H */
