#include "session.h"
#include "kvs_ngtcp2_server.h"
#include <stdlib.h>
#include <string.h>

struct session {
    int fd;
    void *ssl;
    int joined;
    char channel_name[KVS_MAX_CHANNEL_NAME + 1];
    char client_id[KVS_MAX_CLIENT_ID + 1];
    char role[32];
    session_t *peer;
};

#define MAX_CHANNELS 64
static channel_t *channels[MAX_CHANNELS];
static int num_channels;

struct channel {
    char name[KVS_MAX_CHANNEL_NAME + 1];
    session_t *master;
    session_t *viewer;
};

session_t *session_create(int fd, void *ssl) {
    session_t *s = (session_t *)calloc(1, sizeof(session_t));
    if (!s) return NULL;
    s->fd = fd;
    s->ssl = ssl;
    return s;
}

void session_destroy(session_t *s) {
    if (s) free(s);
}

int session_get_fd(const session_t *s) { return s ? s->fd : -1; }
void *session_get_ssl(session_t *s) { return s ? s->ssl : NULL; }

void session_set_joined(session_t *s, const char *channel_name, const char *client_id, const char *role) {
    if (!s) return;
    s->joined = 1;
    strncpy(s->channel_name, channel_name ? channel_name : "", KVS_MAX_CHANNEL_NAME);
    s->channel_name[KVS_MAX_CHANNEL_NAME] = '\0';
    strncpy(s->client_id, client_id ? client_id : "", KVS_MAX_CLIENT_ID);
    s->client_id[KVS_MAX_CLIENT_ID] = '\0';
    strncpy(s->role, role ? role : "", sizeof(s->role) - 1);
    s->role[sizeof(s->role) - 1] = '\0';
}

int session_is_joined(const session_t *s) { return s && s->joined; }

void session_get_channel_name(const session_t *s, char *out, size_t out_len) {
    if (!s || !out || out_len == 0) return;
    strncpy(out, s->channel_name, out_len - 1);
    out[out_len - 1] = '\0';
}

void session_get_client_id(const session_t *s, char *out, size_t out_len) {
    if (!s || !out || out_len == 0) return;
    strncpy(out, s->client_id, out_len - 1);
    out[out_len - 1] = '\0';
}

void session_get_role(const session_t *s, char *out, size_t out_len) {
    if (!s || !out || out_len == 0) return;
    strncpy(out, s->role, out_len - 1);
    out[out_len - 1] = '\0';
}

void session_set_peer(session_t *s, session_t *peer) { if (s) s->peer = peer; }
session_t *session_get_peer(const session_t *s) { return s ? s->peer : NULL; }

static channel_t *channel_create(const char *name) {
    channel_t *ch = (channel_t *)calloc(1, sizeof(channel_t));
    if (!ch) return NULL;
    strncpy(ch->name, name, KVS_MAX_CHANNEL_NAME);
    ch->name[KVS_MAX_CHANNEL_NAME] = '\0';
    return ch;
}

channel_t *channel_find_or_create(const char *channel_name) {
    for (int i = 0; i < num_channels; i++) {
        if (strcmp(channels[i]->name, channel_name) == 0)
            return channels[i];
    }
    if (num_channels >= MAX_CHANNELS) return NULL;
    channel_t *ch = channel_create(channel_name);
    if (ch) channels[num_channels++] = ch;
    return ch;
}

void channel_register(channel_t *ch, session_t *s) {
    if (!ch || !s) return;
    if (strcmp(s->role, KVS_ROLE_MASTER) == 0)
        ch->master = s;
    else
        ch->viewer = s;
    session_t *peer = ch->master && ch->viewer ?
        (s == ch->master ? ch->viewer : ch->master) : NULL;
    if (peer) {
        session_set_peer(s, peer);
        session_set_peer(peer, s);
    }
}

void channel_unregister(channel_t *ch, session_t *s) {
    if (!ch || !s) return;
    if (ch->master == s) ch->master = NULL;
    if (ch->viewer == s) ch->viewer = NULL;
    if (s->peer) {
        s->peer->peer = NULL;
        s->peer = NULL;
    }
}

session_t *channel_get_peer(channel_t *ch, const session_t *s) {
    if (!ch || !s) return NULL;
    return (ch->master == s) ? ch->viewer : ch->master;
}
