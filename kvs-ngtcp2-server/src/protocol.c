#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int find_json_string(const char *json, size_t len, const char *key, char *out, size_t out_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p || (size_t)(p - json) >= len) return -1;
    p += strlen(search);
    while (p < json + len && (*p == ' ' || *p == ':' || *p == ' ')) p++;
    if (p >= json + len || *p != '"') return -1;
    p++;
    const char *start = p;
    while (p < json + len && *p != '"') {
        if (*p == '\\') p++;
        p++;
    }
    if (p >= json + len) return -1;
    size_t n = (size_t)(p - start);
    if (n >= out_len) return -1;
    memcpy(out, start, n);
    out[n] = '\0';
    return 0;
}

int protocol_parse_join(const char *json, size_t len,
    char *channel_name, size_t channel_name_len,
    char *client_id, size_t client_id_len,
    char *role, size_t role_len) {
    if (!json || len == 0) return -1;
    if (find_json_string(json, len, "channelName", channel_name, channel_name_len) != 0) return -1;
    if (find_json_string(json, len, "clientId", client_id, client_id_len) != 0) return -1;
    if (find_json_string(json, len, "role", role, role_len) != 0) return -1;
    return 0;
}

int protocol_build_join_response(const join_response_t *r, char *buf, size_t buf_len) {
    int n;
    if (r->status_code != 200) {
        n = snprintf(buf, buf_len,
            "{\"messageType\":\"JOIN_CHANNEL_RESPONSE\",\"statusCode\":%d,\"errorType\":\"%s\",\"description\":\"%s\"}",
            r->status_code, r->error_type, r->description);
        return (n > 0 && (size_t)n < buf_len) ? n : -1;
    }
    n = snprintf(buf, buf_len,
        "{\"messageType\":\"JOIN_CHANNEL_RESPONSE\",\"statusCode\":200,\"iceServers\":[");
    if (n <= 0 || (size_t)n >= buf_len) return -1;
    size_t off = (size_t)n;
    for (unsigned i = 0; i < r->ice_server_count && off < buf_len; i++) {
        const ice_server_t *s = &r->ice_servers[i];
        n = snprintf(buf + off, buf_len - off,
            "{\"ttl\":%llu,\"uris\":[", (unsigned long long)s->ttl_sec);
        if (n <= 0 || (size_t)n >= buf_len - off) return -1;
        off += (size_t)n;
        for (unsigned j = 0; j < s->uri_count && off < buf_len; j++) {
            n = snprintf(buf + off, buf_len - off, "%s\"%s\"", j ? "," : "", s->uris[j]);
            if (n <= 0 || (size_t)n >= buf_len - off) return -1;
            off += (size_t)n;
        }
        n = snprintf(buf + off, buf_len - off,
            "],\"username\":\"%s\",\"password\":\"%s\"}%s",
            s->username, s->password, (i + 1 < r->ice_server_count) ? "," : "");
        if (n <= 0 || (size_t)n >= buf_len - off) return -1;
        off += (size_t)n;
    }
    n = snprintf(buf + off, buf_len - off, "]}");
    if (n <= 0 || (size_t)n >= buf_len - off) return -1;
    return (int)(off + (size_t)n);
}

static const char *find_value_after_key(const char *json, size_t len, const char *key, size_t *value_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p || (size_t)(p - json) >= len) return NULL;
    p += strlen(search);
    while (p < json + len && (*p == ' ' || *p == ':' || *p == ' ')) p++;
    if (p >= json + len || *p != '"') return NULL;
    p++;
    const char *start = p;
    while (p < json + len && *p != '"') {
        if (*p == '\\') p++;
        p++;
    }
    if (p >= json + len) return NULL;
    *value_len = (size_t)(p - start);
    return start;
}

int protocol_parse_forward(const char *json, size_t len,
    char *message_type, size_t message_type_len,
    char *recipient_id, size_t recipient_id_len,
    const char **payload_start, size_t *payload_len) {
    size_t rlen, mlen;
    const char *m = find_value_after_key(json, len, "action", &mlen);
    if (!m || mlen >= message_type_len) return -1;
    memcpy(message_type, m, mlen);
    message_type[mlen] = '\0';
    const char *r = find_value_after_key(json, len, "RecipientClientId", &rlen);
    if (!r || rlen >= recipient_id_len) return -1;
    memcpy(recipient_id, r, rlen);
    recipient_id[rlen] = '\0';
    const char *pl = find_value_after_key(json, len, "MessagePayload", payload_len);
    if (!pl) return -1;
    *payload_start = pl;
    return 0;
}

int protocol_build_forwarded(const char *sender_id, const char *message_type,
    const char *payload_b64, size_t payload_b64_len,
    char *buf, size_t buf_len) {
    int n = snprintf(buf, buf_len,
        "{\"senderClientId\":\"%s\",\"messageType\":\"%s\",\"messagePayload\":\"%.*s\"}",
        sender_id, message_type, (int)payload_b64_len, payload_b64);
    return (n > 0 && (size_t)n < buf_len) ? n : -1;
}
