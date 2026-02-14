#define LOG_CLASS "KvsNgtcp2Transport"
#include "../Include_i.h"
#include "KvsNgtcp2Transport.h"
#include "LwsApiCalls.h"
#include "Signaling.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket
#define poll WSAPoll
#endif

#define KVS_STREAM_SIGNALING  0
#define KVS_STREAM_AUDIO      1
#define KVS_STREAM_VIDEO      2
#define KVS_FRAME_HEADER_SIZE 5
#define KVS_JOIN_RESPONSE_MAX 4096
#define KVS_ICE_TTL_SEC      86400 /* default ICE config TTL 24h if not in Join response */
#define KVS_RELAY_MEDIA_FRAME_MAX (256 * 1024)
#define RECV_BUF_SIZE        (MAX_SIGNALING_MESSAGE_LEN + KVS_FRAME_HEADER_SIZE + 512)

struct KvsNgtcp2Transport {
    PSignalingClient pClient;
    INT32 sock;
    SSL_CTX* sslCtx;
    SSL* ssl;
    volatile ATOMIC_BOOL running;
    TID recvTid;
    MUTEX sendLock;
    CHAR recvBuf[RECV_BUF_SIZE];
    UINT32 recvLen;
};

static INT32 connectTcp(PCHAR host, PCHAR port) {
    struct addrinfo hints, *res = NULL, *ai;
    INT32 fd = -1;
    CHAR portStr[8];
    SNPRINTF(portStr, sizeof(portStr), "%s", port);
    MEMSET(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portStr, &hints, &res) != 0) return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = (INT32)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int sendFrame(PKvsNgtcp2Transport pTransport, UINT8 streamId, const void* payload, UINT32 len) {
    UINT8 hdr[KVS_FRAME_HEADER_SIZE];
    hdr[0] = streamId;
    hdr[1] = (UINT8)((len >> 24) & 0xff);
    hdr[2] = (UINT8)((len >> 16) & 0xff);
    hdr[3] = (UINT8)((len >> 8) & 0xff);
    hdr[4] = (UINT8)(len & 0xff);
    MUTEX_LOCK(pTransport->sendLock);
    if (SSL_write(pTransport->ssl, hdr, KVS_FRAME_HEADER_SIZE) != KVS_FRAME_HEADER_SIZE) {
        MUTEX_UNLOCK(pTransport->sendLock);
        return -1;
    }
    if (len && SSL_write(pTransport->ssl, payload, (int)len) != (int)len) {
        MUTEX_UNLOCK(pTransport->sendLock);
        return -1;
    }
    MUTEX_UNLOCK(pTransport->sendLock);
    return 0;
}

static int recvExact(PKvsNgtcp2Transport pTransport, void* buf, UINT32 len) {
    UINT32 got = 0;
    while (got < len) {
        int n = SSL_read(pTransport->ssl, (char*)buf + got, (int)(len - got));
        if (n <= 0) return -1;
        got += (UINT32)n;
    }
    return 0;
}

/* Parse JoinChannel response JSON and fill iceConfigs. */
static STATUS parseJoinResponseAndFillIce(PSignalingClient pClient, const char* json, UINT32 len) {
    /* Minimal parser: find "iceServers" array and first entry's uris, username, password, ttl */
    const char* p = strstr(json, "\"iceServers\"");
    if (!p || (size_t)(p - json) >= len) return STATUS_SIGNALING_GET_ICE_CONFIG_CALL_FAILED;
    p = strchr(p, '[');
    if (!p) return STATUS_SIGNALING_GET_ICE_CONFIG_CALL_FAILED;
    p = strchr(p, '{');
    if (!p) return STATUS_SIGNALING_GET_ICE_CONFIG_CALL_FAILED;
    /* First object: ttl, uris[], username, password */
    pClient->iceConfigCount = 1;
    PIceConfigInfo info = &pClient->iceConfigs[0];
    info->version = SIGNALING_ICE_CONFIG_INFO_CURRENT_VERSION;
    info->ttl = KVS_ICE_TTL_SEC * HUNDREDS_OF_NANOS_IN_A_SECOND;
    info->uriCount = 0;
    MEMSET(info->uris, 0, sizeof(info->uris));
    MEMSET(info->userName, 0, sizeof(info->userName));
    MEMSET(info->password, 0, sizeof(info->password));
    const char* end = json + len;
    const char* key;
    while (p < end && *p) {
        if (*p == '"') {
            key = p + 1;
            p = strchr(p + 1, '"');
            if (!p) break;
            p++;
            while (p < end && (*p == ' ' || *p == ':')) p++;
            if (p >= end) break;
            if (*p == '"') {
                p++;
                const char* val = p;
                p = strchr(p, '"');
                if (!p) break;
                size_t vlen = (size_t)(p - val);
                if (vlen >= 2 && key[0] == 't' && key[1] == 't' && key[2] == 'l') {
                    info->ttl = (UINT64)atoi(val) * HUNDREDS_OF_NANOS_IN_A_SECOND;
                } else if (vlen >= 4 && key[0] == 'u' && key[1] == 's' && key[2] == 'e' && key[3] == 'r') {
                    UINT32 ul = (UINT32)(vlen < (size_t)MAX_ICE_CONFIG_USER_NAME_LEN ? vlen : (size_t)MAX_ICE_CONFIG_USER_NAME_LEN);
                    STRNCPY(info->userName, val, ul);
                    info->userName[ul < MAX_ICE_CONFIG_USER_NAME_BUFFER_LEN ? ul : MAX_ICE_CONFIG_USER_NAME_LEN] = '\0';
                } else if (vlen >= 4 && key[0] == 'p' && key[1] == 'a' && key[2] == 's' && key[3] == 's') {
                    UINT32 pl = (UINT32)(vlen < (size_t)MAX_ICE_CONFIG_CREDENTIAL_LEN ? vlen : (size_t)MAX_ICE_CONFIG_CREDENTIAL_LEN);
                    STRNCPY(info->password, val, pl);
                    info->password[pl < MAX_ICE_CONFIG_CREDENTIAL_BUFFER_LEN ? pl : MAX_ICE_CONFIG_CREDENTIAL_LEN] = '\0';
                }
                p++;
            } else if (*p == '[') {
                p++;
                while (p < end && *p != ']') {
                    if (*p == '"') {
                        p++;
                        const char* uri = p;
                        p = strchr(p, '"');
                        if (!p) break;
                        size_t ulen = (size_t)(p - uri);
                        if (info->uriCount < MAX_ICE_CONFIG_URI_COUNT && ulen < MAX_ICE_CONFIG_URI_BUFFER_LEN) {
                            UINT32 uilen = (UINT32)(ulen < (size_t)MAX_ICE_CONFIG_URI_LEN ? ulen : (size_t)MAX_ICE_CONFIG_URI_LEN);
                            STRNCPY(info->uris[info->uriCount], uri, uilen);
                            info->uris[info->uriCount][uilen < MAX_ICE_CONFIG_URI_BUFFER_LEN ? uilen : MAX_ICE_CONFIG_URI_LEN] = '\0';
                            info->uriCount++;
                        }
                        p++;
                    } else
                        p++;
                }
                if (p < end) p++;
            }
        } else
            p++;
    }
    pClient->iceConfigTime = GETTIME();
    pClient->iceConfigExpiration = pClient->iceConfigTime + info->ttl;
    return STATUS_SUCCESS;
}

STATUS kvsNgtcp2TransportCreate(PSignalingClient pSignalingClient, PKvsNgtcp2Transport* ppTransport) {
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsNgtcp2Transport pTransport = NULL;
    CHK(pSignalingClient != NULL && ppTransport != NULL, STATUS_NULL_ARG);
    CHK(NULL != (pTransport = (PKvsNgtcp2Transport)MEMCALLOC(1, sizeof(*pTransport))), STATUS_NOT_ENOUGH_MEMORY);
    pTransport->pClient = pSignalingClient;
    pTransport->sock = -1;
    pTransport->sslCtx = NULL;
    pTransport->ssl = NULL;
    ATOMIC_STORE_BOOL(&pTransport->running, FALSE);
    pTransport->recvTid = INVALID_TID_VALUE;
    pTransport->sendLock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pTransport->sendLock), STATUS_INVALID_OPERATION);
    *ppTransport = pTransport;
CleanUp:
    LEAVES();
    return retStatus;
}

void kvsNgtcp2TransportDestroy(PKvsNgtcp2Transport* ppTransport) {
    if (!ppTransport || !*ppTransport) return;
    PKvsNgtcp2Transport p = *ppTransport;
    ATOMIC_STORE_BOOL(&p->running, FALSE);
    if (p->recvTid != INVALID_TID_VALUE) {
        THREAD_JOIN(p->recvTid, NULL);
        p->recvTid = INVALID_TID_VALUE;
    }
    if (p->ssl) { SSL_shutdown(p->ssl); SSL_free(p->ssl); p->ssl = NULL; }
    if (p->sslCtx) { SSL_CTX_free(p->sslCtx); p->sslCtx = NULL; }
    if (p->sock >= 0) { close(p->sock); p->sock = -1; }
    MUTEX_FREE(p->sendLock);
    SAFE_MEMFREE(*ppTransport);
}

static PVOID recvThread(PVOID arg) {
    PKvsNgtcp2Transport pTransport = (PKvsNgtcp2Transport)arg;
    PSignalingClient pClient = pTransport->pClient;
    UINT8 hdr[KVS_FRAME_HEADER_SIZE];
    CHAR* msgBuf = NULL;
    PBYTE mediaBuf = NULL;
    UINT32 msgLen = 0;
    msgBuf = (CHAR*)MEMALLOC(MAX_SIGNALING_MESSAGE_LEN + 1);
    if (!msgBuf) return NULL;
    while (ATOMIC_LOAD_BOOL(&pTransport->running)) {
        if (recvExact(pTransport, hdr, KVS_FRAME_HEADER_SIZE) != 0) break;
        msgLen = (UINT32)hdr[1] << 24 | (UINT32)hdr[2] << 16 | (UINT32)hdr[3] << 8 | hdr[4];
        if (hdr[0] == KVS_STREAM_SIGNALING) {
            if (msgLen == 0 || msgLen > MAX_SIGNALING_MESSAGE_LEN) continue;
            if (recvExact(pTransport, msgBuf, msgLen) != 0) break;
            msgBuf[msgLen] = '\0';
            ReceivedSignalingMessage received;
            MEMSET(&received, 0, sizeof(received));
            if (parseSignalingMessage(msgBuf, msgLen, &received) == STATUS_SUCCESS &&
                pClient->signalingClientCallbacks.messageReceivedFn) {
                pClient->signalingClientCallbacks.messageReceivedFn(
                    pClient->signalingClientCallbacks.customData, &received);
            }
        } else if ((hdr[0] == KVS_STREAM_AUDIO || hdr[0] == KVS_STREAM_VIDEO) &&
                   pClient->signalingClientCallbacks.relayMediaReceivedFn &&
                   msgLen > 0 && msgLen <= KVS_RELAY_MEDIA_FRAME_MAX) {
            mediaBuf = (PBYTE)MEMALLOC(msgLen);
            if (mediaBuf && recvExact(pTransport, mediaBuf, msgLen) == 0) {
                pClient->signalingClientCallbacks.relayMediaReceivedFn(
                    pClient->signalingClientCallbacks.customData,
                    (hdr[0] == KVS_STREAM_VIDEO),
                    mediaBuf, msgLen);
            }
            SAFE_MEMFREE(mediaBuf);
            mediaBuf = NULL;
        } else if (msgLen > 0 && msgLen <= KVS_RELAY_MEDIA_FRAME_MAX) {
            /* Skip payload for unknown stream or when no media callback */
            mediaBuf = (PBYTE)MEMALLOC(msgLen);
            if (mediaBuf) recvExact(pTransport, mediaBuf, msgLen);
            SAFE_MEMFREE(mediaBuf);
            mediaBuf = NULL;
        }
    }
    SAFE_MEMFREE(msgBuf);
    return NULL;
}

STATUS kvsNgtcp2TransportConnect(PKvsNgtcp2Transport pTransport, PCHAR host, PCHAR port) {
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSignalingClient pClient;
    CHAR joinJson[512];
    UINT32 joinLen;
    CHAR respBuf[KVS_JOIN_RESPONSE_MAX];
    UINT32 respLen;
    UINT8 hdr[KVS_FRAME_HEADER_SIZE];
    CHK(pTransport != NULL && host != NULL && port != NULL, STATUS_NULL_ARG);
    pClient = pTransport->pClient;
    CHK(pClient != NULL, STATUS_NULL_ARG);
    CHK((pTransport->sock = connectTcp(host, port)) >= 0, STATUS_SIGNALING_CONNECT_CALL_FAILED);
    pTransport->sslCtx = SSL_CTX_new(TLS_client_method());
    CHK(pTransport->sslCtx != NULL, STATUS_SIGNALING_CONNECT_CALL_FAILED);
    pTransport->ssl = SSL_new(pTransport->sslCtx);
    CHK(pTransport->ssl != NULL, STATUS_SIGNALING_CONNECT_CALL_FAILED);
    SSL_set_fd(pTransport->ssl, pTransport->sock);
    if (SSL_connect(pTransport->ssl) <= 0) {
        DLOGE("TLS connect failed");
        CHK(FALSE, STATUS_SIGNALING_CONNECT_CALL_FAILED);
    }
    {
        PCHAR pChan = (pClient->pChannelInfo && pClient->pChannelInfo->pChannelName) ? pClient->pChannelInfo->pChannelName : "";
        PCHAR pCid = (pClient->clientInfo.signalingClientInfo.clientId && pClient->clientInfo.signalingClientInfo.clientId[0]) ? pClient->clientInfo.signalingClientInfo.clientId : "";
        joinLen = (UINT32)SNPRINTF(joinJson, sizeof(joinJson),
            "{\"messageType\":\"JOIN_CHANNEL\",\"channelName\":\"%s\",\"clientId\":\"%s\",\"role\":\"%s\"}",
            pChan, pCid,
            (pClient->pChannelInfo && pClient->pChannelInfo->channelRoleType == SIGNALING_CHANNEL_ROLE_TYPE_MASTER) ? "master" : "viewer");
    }
    CHK(sendFrame(pTransport, (UINT8)KVS_STREAM_SIGNALING, joinJson, joinLen) == 0, STATUS_SIGNALING_CONNECT_CALL_FAILED);
    if (recvExact(pTransport, hdr, KVS_FRAME_HEADER_SIZE) != 0) CHK(FALSE, STATUS_SIGNALING_CONNECT_CALL_FAILED);
    respLen = (UINT32)hdr[1] << 24 | (UINT32)hdr[2] << 16 | (UINT32)hdr[3] << 8 | hdr[4];
    CHK(respLen > 0 && respLen < KVS_JOIN_RESPONSE_MAX, STATUS_SIGNALING_CONNECT_CALL_FAILED);
    if (recvExact(pTransport, respBuf, respLen) != 0) CHK(FALSE, STATUS_SIGNALING_CONNECT_CALL_FAILED);
    respBuf[respLen] = '\0';
    if (strstr(respBuf, "\"statusCode\":200") == NULL) {
        DLOGE("JoinChannel failed: %.*s", (int)respLen, respBuf);
        CHK(FALSE, STATUS_SIGNALING_CONNECT_CALL_FAILED);
    }
    CHK_STATUS(parseJoinResponseAndFillIce(pClient, respBuf, respLen));
    ATOMIC_STORE_BOOL(&pTransport->running, TRUE);
    THREAD_CREATE(&pTransport->recvTid, recvThread, pTransport);
    ATOMIC_STORE_BOOL(&pClient->connected, TRUE);
CleanUp:
    if (STATUS_FAILED(retStatus)) {
        if (pTransport->ssl) { SSL_free(pTransport->ssl); pTransport->ssl = NULL; }
        if (pTransport->sslCtx) { SSL_CTX_free(pTransport->sslCtx); pTransport->sslCtx = NULL; }
        if (pTransport->sock >= 0) { close(pTransport->sock); pTransport->sock = -1; }
    }
    LEAVES();
    return retStatus;
}

STATUS kvsNgtcp2TransportSendMessage(PKvsNgtcp2Transport pTransport, SIGNALING_MESSAGE_TYPE messageType,
    PCHAR peerClientId, PCHAR pPayload, UINT32 payloadLen, PCHAR pCorrelationId, UINT32 correlationIdLen) {
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    CHAR encoded[MAX_SIGNALING_MESSAGE_LEN + 256];
    UINT32 encodedLen = (UINT32)ARRAY_SIZE(encoded);
    CHAR sendBuf[MAX_SIGNALING_MESSAGE_LEN + 1024];
    INT32 sendLen;
    PCHAR pMsgType = getMessageTypeInString(messageType);
    CHK(pTransport != NULL && peerClientId != NULL && pPayload != NULL, STATUS_NULL_ARG);
    CHK(pMsgType != NULL, STATUS_INVALID_ARG);
    if (payloadLen == 0) payloadLen = (UINT32)STRLEN(pPayload);
    CHK_STATUS(base64Encode((PBYTE)pPayload, payloadLen, encoded, (PUINT32)&encodedLen));
    if (correlationIdLen == 0 && pCorrelationId) correlationIdLen = (UINT32)STRLEN(pCorrelationId);
    if (pCorrelationId && correlationIdLen > 0)
        sendLen = SNPRINTF(sendBuf, sizeof(sendBuf),
            "{\"action\":\"%s\",\"RecipientClientId\":\"%.*s\",\"MessagePayload\":\"%s\",\"CorrelationId\":\"%.*s\"}",
            pMsgType, MAX_SIGNALING_CLIENT_ID_LEN, peerClientId, encoded, correlationIdLen, pCorrelationId);
    else
        sendLen = SNPRINTF(sendBuf, sizeof(sendBuf),
            "{\"action\":\"%s\",\"RecipientClientId\":\"%.*s\",\"MessagePayload\":\"%s\"}",
            pMsgType, MAX_SIGNALING_CLIENT_ID_LEN, peerClientId, encoded);
    CHK(sendLen > 0 && (UINT32)sendLen < sizeof(sendBuf), STATUS_INVALID_ARG);
    CHK(sendFrame(pTransport, (UINT8)KVS_STREAM_SIGNALING, sendBuf, (UINT32)sendLen) == 0, STATUS_SIGNALING_MESSAGE_DELIVERY_FAILED);
CleanUp:
    LEAVES();
    return retStatus;
}

STATUS kvsNgtcp2TransportSendMediaFrame(PKvsNgtcp2Transport pTransport, BOOL isVideo, PBYTE pData, UINT32 dataLen) {
    if (!pTransport || !pData) return STATUS_NULL_ARG;
    UINT8 streamId = isVideo ? (UINT8)KVS_STREAM_VIDEO : (UINT8)KVS_STREAM_AUDIO;
    return (sendFrame(pTransport, streamId, pData, dataLen) == 0) ? STATUS_SUCCESS : STATUS_SIGNALING_MESSAGE_DELIVERY_FAILED;
}

void kvsNgtcp2TransportDisconnect(PKvsNgtcp2Transport pTransport) {
    if (!pTransport) return;
    ATOMIC_STORE_BOOL(&pTransport->running, FALSE);
    if (pTransport->recvTid != INVALID_TID_VALUE) {
        THREAD_JOIN(pTransport->recvTid, NULL);
        pTransport->recvTid = INVALID_TID_VALUE;
    }
    if (pTransport->ssl) { SSL_shutdown(pTransport->ssl); SSL_free(pTransport->ssl); pTransport->ssl = NULL; }
    if (pTransport->sslCtx) { SSL_CTX_free(pTransport->sslCtx); pTransport->sslCtx = NULL; }
    if (pTransport->sock >= 0) { close(pTransport->sock); pTransport->sock = -1; }
    if (pTransport->pClient) ATOMIC_STORE_BOOL(&pTransport->pClient->connected, FALSE);
}
