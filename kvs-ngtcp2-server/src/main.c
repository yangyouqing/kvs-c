#include "kvs_ngtcp2_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [--bind ADDR] [--port PORT] [--cert FILE] [--key FILE] [--stun URI] [--turn URI] [--turn-user U] [--turn-pass P]\n", prog);
    fprintf(stderr, "  Default: 0.0.0.0:4433, cert.pem, key.pem; STUN/TURN from env or defaults.\n");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    server_config_t config = {
        .listen_addr = "0.0.0.0",
        .listen_port = "4433",
        .cert_file = "cert.pem",
        .key_file = "key.pem",
        .stun_uri = NULL,
        .turn_uri = NULL,
        .turn_username = NULL,
        .turn_password = NULL,
    };

    const char *stun = getenv("KVS_STUN_URI");
    const char *turn = getenv("KVS_TURN_URI");
    const char *turn_user = getenv("KVS_TURN_USER");
    const char *turn_pass = getenv("KVS_TURN_PASS");
    if (stun) config.stun_uri = stun;
    if (turn) config.turn_uri = turn;
    if (turn_user) config.turn_username = turn_user;
    if (turn_pass) config.turn_password = turn_pass;
    if (!config.stun_uri) config.stun_uri = "stun:127.0.0.1:3478";
    if (!config.turn_uri) config.turn_uri = "turn:127.0.0.1:3478?transport=udp";
    if (!config.turn_username) config.turn_username = "user";
    if (!config.turn_password) config.turn_password = "pass";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) { config.listen_addr = argv[++i]; continue; }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) { config.listen_port = argv[++i]; continue; }
        if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) { config.cert_file = argv[++i]; continue; }
        if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) { config.key_file = argv[++i]; continue; }
        if (strcmp(argv[i], "--stun") == 0 && i + 1 < argc) { config.stun_uri = argv[++i]; continue; }
        if (strcmp(argv[i], "--turn") == 0 && i + 1 < argc) { config.turn_uri = argv[++i]; continue; }
        if (strcmp(argv[i], "--turn-user") == 0 && i + 1 < argc) { config.turn_username = argv[++i]; continue; }
        if (strcmp(argv[i], "--turn-pass") == 0 && i + 1 < argc) { config.turn_password = argv[++i]; continue; }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
    }

    if (server_run(&config) != 0) {
        fprintf(stderr, "server_run failed\n");
        return 1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
