#ifndef MATON_DNS_FORWARDER_CORE_H
#define MATON_DNS_FORWARDER_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#define DNS_FORWARDER_MAX_PACKET 65535
#define DNS_FORWARDER_TCP_PREFIX 2

typedef int (*dns_uid_lookup_fn)(void *context, const void *peer, uint32_t *uid);

struct dns_udp_diag_socket {
    uint32_t uid;
    struct in_addr local_address;
    uint16_t local_port;
    struct in_addr remote_address;
    uint16_t remote_port;
};

struct dns_token_bucket {
    double tokens;
    double capacity;
    double tokens_per_second;
    uint64_t last_ns;
};

int dns_forwarder_uid_allowed(dns_uid_lookup_fn lookup, void *context,
                              const void *peer, uint32_t expected_uid);
void dns_token_bucket_init(struct dns_token_bucket *bucket, double capacity,
                           double tokens_per_second, uint64_t now_ns);
int dns_token_bucket_take(struct dns_token_bucket *bucket, uint64_t now_ns);
int dns_tcp_frame_length(const uint8_t prefix[2], size_t *frame_length);
int dns_tcp_frame_write(uint8_t *output, size_t capacity, const uint8_t *message,
                        size_t message_length, size_t *written);
void dns_forwarder_address(uint32_t uid, char output[16]);
int dns_forwarder_udp_uid(const struct dns_udp_diag_socket *sockets, size_t count,
                          struct in_addr source_address, uint16_t source_port,
                          struct in_addr destination_address, uint16_t destination_port,
                          uint32_t *uid);

#endif
