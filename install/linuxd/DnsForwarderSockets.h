#ifndef MATONOS_LINUXD_DNS_FORWARDER_SOCKETS_H
#define MATONOS_LINUXD_DNS_FORWARDER_SOCKETS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int matonos_dns_create_sockets(const char *address, uint32_t uid,
                               int *udp_fd, int *tcp_fd);

#ifdef __cplusplus
}
#endif

#endif
