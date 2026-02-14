#ifndef KVS_NGTCP2_PROTOCOL_H
#define KVS_NGTCP2_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame: 1 byte stream_id + 4 byte length (big-endian) + payload */
#define KVS_FRAME_HEADER_SIZE 5
#define KVS_MAX_FRAME_PAYLOAD (1u * 1024 * 1024)  /* 1MB */

typedef struct ice_server {
    uint64_t ttl_sec;
    char uris[4][128];
    unsigned uri_count;
    char username[257];
    char password[257];
} ice_server_t;

typedef struct join_response {
    int status_code;
    char error_type[64];
    char description[256];
    ice_server_t ice_servers[2];
    unsigned ice_server_count;
} join_response_t;

/* Parse JoinChannel request (JSON); returns 0 on success */
int protocol_parse_join(const char *json, size_t len,
    char *channel_name, size_t channel_name_len,
    char *client_id, size_t client_id_len,
    char *role, size_t role_len);

/* Build JoinChannel response (JSON) into buf; returns length or -1 on error */
int protocol_build_join_response(const join_response_t *r, char *buf, size_t buf_len);

/* Parse application signaling: read "action", "RecipientClientId", "MessagePayload" for forwarding */
int protocol_parse_forward(const char *json, size_t len,
    char *message_type, size_t message_type_len,
    char *recipient_id, size_t recipient_id_len,
    const char **payload_start, size_t *payload_len);

/* Build forwarded message: senderClientId + messageType + messagePayload (so client parseSignalingMessage works) */
int protocol_build_forwarded(const char *sender_id, const char *message_type,
    const char *payload_b64, size_t payload_b64_len,
    char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* KVS_NGTCP2_PROTOCOL_H */
