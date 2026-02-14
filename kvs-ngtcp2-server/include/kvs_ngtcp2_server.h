#ifndef KVS_NGTCP2_SERVER_H
#define KVS_NGTCP2_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#define KVS_STREAM_SIGNALING  0
#define KVS_STREAM_AUDIO      1
#define KVS_STREAM_VIDEO      2

#define KVS_MAX_CHANNEL_NAME  256
#define KVS_MAX_CLIENT_ID     256
#define KVS_MAX_SIGNALING_MSG 18750
#define KVS_MAX_ICE_URIS      4
#define KVS_MAX_ICE_URI_LEN   127
#define KVS_ICE_TTL_SEC       86400

typedef struct server_config {
    const char *listen_addr;
    const char *listen_port;
    const char *cert_file;
    const char *key_file;
    const char *stun_uri;
    const char *turn_uri;
    const char *turn_username;
    const char *turn_password;
} server_config_t;

int server_run(const server_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
