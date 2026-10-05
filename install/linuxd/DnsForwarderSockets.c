#define _GNU_SOURCE
#include "DnsForwarderSockets.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int matonos_dns_create_sockets(const char *address, uint32_t uid,
                               int *udp_fd, int *tcp_fd) {
    if (address == NULL || udp_fd == NULL || tcp_fd == NULL) return -EINVAL;
    *udp_fd = -1;
    *tcp_fd = -1;
    char expected[16];
    snprintf(expected, sizeof(expected), "127.%u.%u.%u",
             10 + ((uid >> 16) & 0x3f), (uid >> 8) & 0xff, uid & 0xff);
    if (strcmp(address, expected) != 0) return -EINVAL;
    struct sockaddr_in bind_address = {.sin_family = AF_INET, .sin_port = htons(53)};
    if (inet_pton(AF_INET, expected, &bind_address.sin_addr) != 1) return -EINVAL;

    int udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (udp < 0) return -errno;
    if (bind(udp, (struct sockaddr *)&bind_address, sizeof(bind_address)) != 0) {
        int saved = errno;
        close(udp);
        return -saved;
    }
    int tcp = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (tcp < 0) {
        int saved = errno;
        close(udp);
        return -saved;
    }
    if (bind(tcp, (struct sockaddr *)&bind_address, sizeof(bind_address)) != 0 ||
        listen(tcp, 16) != 0) {
        int saved = errno;
        close(tcp);
        close(udp);
        return -saved;
    }
    *udp_fd = udp;
    *tcp_fd = tcp;
    return 0;
}
