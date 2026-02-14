#ifndef KVS_NGTCP2_SESSION_H
#define KVS_NGTCP2_SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVS_ROLE_MASTER  "master"
#define KVS_ROLE_VIEWER  "viewer"

typedef struct session session_t;
typedef struct channel channel_t;

/* Session: one client connection (channelName, clientId, role, SSL*, peer reference) */
session_t *session_create(int fd, void *ssl);
void session_destroy(session_t *s);
int session_get_fd(const session_t *s);
void *session_get_ssl(session_t *s);

void session_set_joined(session_t *s, const char *channel_name, const char *client_id, const char *role);
int session_is_joined(const session_t *s);
void session_get_channel_name(const session_t *s, char *out, size_t out_len);
void session_get_client_id(const session_t *s, char *out, size_t out_len);
void session_get_role(const session_t *s, char *out, size_t out_len);

/* Peer in same channel (for forwarding) */
void session_set_peer(session_t *s, session_t *peer);
session_t *session_get_peer(const session_t *s);

/* Channel: holds master + viewer sessions */
channel_t *channel_find_or_create(const char *channel_name);
void channel_register(channel_t *ch, session_t *s);
void channel_unregister(channel_t *ch, session_t *s);
session_t *channel_get_peer(channel_t *ch, const session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* KVS_NGTCP2_SESSION_H */
