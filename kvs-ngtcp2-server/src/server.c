#include "kvs_ngtcp2_server.h"
#include "session.h"
#include "protocol.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#define SOCKET_CLOSE(fd) close(fd)
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <pthread.h>
#pragma comment(lib, "ws2_32.lib")
#define SOCKET_CLOSE(fd) closesocket(fd)
#endif

#define KVS_FRAME_HEADER_SIZE 5
#define RECV_BUF_SIZE (KVS_MAX_SIGNALING_MSG + KVS_FRAME_HEADER_SIZE + 512)

static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;
static const server_config_t *g_config;

static int send_frame(SSL *ssl, unsigned char stream_id, const void *payload, unsigned int len)
{
    unsigned char hdr[KVS_FRAME_HEADER_SIZE];
    hdr[0] = (unsigned char)stream_id;
    hdr[1] = (unsigned char)((len >> 24) & 0xff);
    hdr[2] = (unsigned char)((len >> 16) & 0xff);
    hdr[3] = (unsigned char)((len >> 8) & 0xff);
    hdr[4] = (unsigned char)(len & 0xff);
    pthread_mutex_lock(&g_send_mutex);
    if (SSL_write(ssl, hdr, KVS_FRAME_HEADER_SIZE) != KVS_FRAME_HEADER_SIZE) {
        pthread_mutex_unlock(&g_send_mutex);
        return -1;
    }
    if (len && SSL_write(ssl, payload, (int)len) != (int)len) {
        pthread_mutex_unlock(&g_send_mutex);
        return -1;
    }
    pthread_mutex_unlock(&g_send_mutex);
    return 0;
}

static int recv_exact(SSL *ssl, void *buf, unsigned int len)
{
    unsigned int got = 0;
    while (got < len) {
        int n = SSL_read(ssl, (char *)buf + got, (int)(len - got));
        if (n <= 0) return -1;
        got += (unsigned int)n;
    }
    return 0;
}

static void fill_ice_from_config(const server_config_t *config, join_response_t *r)
{
    r->ice_server_count = 1;
    ice_server_t *s = &r->ice_servers[0];
    s->ttl_sec = KVS_ICE_TTL_SEC;
    s->uri_count = 0;
    memset(s->uris, 0, sizeof(s->uris));
    if (config->stun_uri && strlen(config->stun_uri) < sizeof(s->uris[0])) {
        strncpy(s->uris[s->uri_count], config->stun_uri, sizeof(s->uris[0]) - 1);
        s->uri_count++;
    }
    if (config->turn_uri && strlen(config->turn_uri) < sizeof(s->uris[0]) && s->uri_count < KVS_MAX_ICE_URIS) {
        strncpy(s->uris[s->uri_count], config->turn_uri, sizeof(s->uris[0]) - 1);
        s->uri_count++;
    }
    if (config->turn_username)
        strncpy(s->username, config->turn_username, sizeof(s->username) - 1);
    else
        s->username[0] = '\0';
    if (config->turn_password)
        strncpy(s->password, config->turn_password, sizeof(s->password) - 1);
    else
        s->password[0] = '\0';
}

static void *client_thread(void *arg)
{
    session_t *s = (session_t *)arg;
    int fd = session_get_fd(s);
    SSL *ssl = (SSL *)session_get_ssl(s);
    channel_t *ch = NULL;
    char *msg_buf = NULL;
    (void)fd;

    msg_buf = (char *)malloc(RECV_BUF_SIZE);
    if (!msg_buf) goto done;

    for (;;) {
        unsigned char hdr[KVS_FRAME_HEADER_SIZE];
        if (recv_exact(ssl, hdr, KVS_FRAME_HEADER_SIZE) != 0) break;
        unsigned int payload_len = (unsigned int)hdr[1] << 24 | (unsigned int)hdr[2] << 16 | (unsigned int)hdr[3] << 8 | hdr[4];
        if (payload_len == 0 || payload_len > RECV_BUF_SIZE - KVS_FRAME_HEADER_SIZE) break;
        if (recv_exact(ssl, msg_buf, payload_len) != 0) break;
        msg_buf[payload_len] = '\0';

        unsigned char stream_id = hdr[0];
        if (stream_id == KVS_STREAM_SIGNALING) {
            if (strstr(msg_buf, "\"messageType\":\"JOIN_CHANNEL\"") || strstr(msg_buf, "\"messageType\": \"JOIN_CHANNEL\"")) {
                char channel_name[KVS_MAX_CHANNEL_NAME + 1], client_id[KVS_MAX_CLIENT_ID + 1], role[32];
                if (protocol_parse_join(msg_buf, payload_len, channel_name, sizeof(channel_name), client_id, sizeof(client_id), role, sizeof(role)) == 0) {
                    const server_config_t *config = g_config;
                    if (!config) continue;
                    join_response_t jr;
                    memset(&jr, 0, sizeof(jr));
                    jr.status_code = 200;
                    fill_ice_from_config(config, &jr);
                    char resp_buf[4096];
                    int resp_len = protocol_build_join_response(&jr, resp_buf, sizeof(resp_buf));
                    if (resp_len > 0) {
                        send_frame(ssl, KVS_STREAM_SIGNALING, resp_buf, (unsigned int)resp_len);
                        session_set_joined(s, channel_name, client_id, role);
                        ch = channel_find_or_create(channel_name);
                        if (ch) channel_register(ch, s);
                    }
                }
            } else if (session_is_joined(s)) {
                char msg_type[64], recipient_id[KVS_MAX_CLIENT_ID + 1];
                const char *payload_start;
                size_t payload_len_s;
                if (protocol_parse_forward(msg_buf, payload_len, msg_type, sizeof(msg_type), recipient_id, sizeof(recipient_id), &payload_start, &payload_len_s) == 0) {
                    session_t *peer = channel_get_peer(ch, s);
                    if (peer) {
                        SSL *peer_ssl = (SSL *)session_get_ssl(peer);
                        char sender_id[KVS_MAX_CLIENT_ID + 1];
                        session_get_client_id(s, sender_id, sizeof(sender_id));
                        char fwd_buf[KVS_MAX_SIGNALING_MSG + 1024];
                        int fwd_len = protocol_build_forwarded(sender_id, msg_type, payload_start, payload_len_s, fwd_buf, sizeof(fwd_buf));
                        if (fwd_len > 0 && peer_ssl)
                            send_frame(peer_ssl, KVS_STREAM_SIGNALING, fwd_buf, (unsigned int)fwd_len);
                    }
                }
            }
        } else if ((stream_id == KVS_STREAM_AUDIO || stream_id == KVS_STREAM_VIDEO) && session_is_joined(s) && ch) {
            session_t *peer = channel_get_peer(ch, s);
            if (peer) {
                SSL *peer_ssl = (SSL *)session_get_ssl(peer);
                if (peer_ssl) send_frame(peer_ssl, stream_id, msg_buf, payload_len);
            }
        }
    }
done:
    if (ch) channel_unregister(ch, s);
    free(msg_buf);
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    SOCKET_CLOSE(fd);
    session_destroy(s);
    return NULL;
}

int server_run(const server_config_t *config)
{
    if (!config || !config->listen_addr || !config->listen_port) return -1;

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return -1;
    if (SSL_CTX_use_certificate_file(ctx, config->cert_file, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, config->key_file, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx);
        return -1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { SSL_CTX_free(ctx); return -1; }
    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&on, sizeof(on));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)atoi(config->listen_port));
    if (strcmp(config->listen_addr, "0.0.0.0") == 0)
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, config->listen_addr, &addr.sin_addr);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        SOCKET_CLOSE(listen_fd);
        SSL_CTX_free(ctx);
        return -1;
    }
    if (listen(listen_fd, 64) != 0) {
        SOCKET_CLOSE(listen_fd);
        SSL_CTX_free(ctx);
        return -1;
    }
    fprintf(stderr, "kvs-ngtcp2-server listening on %s:%s (TLS)\n", config->listen_addr, config->listen_port);
    g_config = config;

    for (;;) {
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_len);
        if (client_fd < 0) continue;
        SSL *ssl = SSL_new(ctx);
        if (!ssl) { SOCKET_CLOSE(client_fd); continue; }
        SSL_set_fd(ssl, client_fd);
        if (SSL_accept(ssl) <= 0) {
            SSL_free(ssl);
            SOCKET_CLOSE(client_fd);
            continue;
        }
        session_t *s = session_create(client_fd, ssl);
        if (!s) { SSL_free(ssl); SOCKET_CLOSE(client_fd); continue; }
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, s) != 0) {
            session_destroy(s);
            SSL_free(ssl);
            SOCKET_CLOSE(client_fd);
            continue;
        }
        pthread_detach(tid);
    }
}
