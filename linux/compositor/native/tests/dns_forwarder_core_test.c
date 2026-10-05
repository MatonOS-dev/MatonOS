#include "../dns_forwarder_core.h"

#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

struct mock_diag { int result; uint32_t uid; };
static int mock_uid(void *context, const void *peer, uint32_t *uid) {
    struct mock_diag *diag = context;
    if (peer != diag || diag->result != 0) return -1;
    *uid = diag->uid;
    return 0;
}

int main(void) {
    struct mock_diag diag = {0, 10123};
    assert(dns_forwarder_uid_allowed(mock_uid, &diag, &diag, 10123));
    assert(!dns_forwarder_uid_allowed(mock_uid, &diag, &diag, 10124));
    assert(!dns_forwarder_uid_allowed(mock_uid, &diag, &diag, 0));
    diag.result = -1;
    assert(!dns_forwarder_uid_allowed(mock_uid, &diag, &diag, 10123));
    assert(!dns_forwarder_uid_allowed(NULL, &diag, &diag, 10123));

    struct dns_token_bucket bucket;
    dns_token_bucket_init(&bucket, 2, 1, 1000000000ULL);
    assert(dns_token_bucket_take(&bucket, 1000000000ULL));
    assert(dns_token_bucket_take(&bucket, 1000000000ULL));
    assert(!dns_token_bucket_take(&bucket, 1000000000ULL));
    assert(dns_token_bucket_take(&bucket, 2000000000ULL));

    uint8_t prefix[2] = {0x01, 0x00};
    size_t length = 0;
    assert(dns_tcp_frame_length(prefix, &length) == 0 && length == 256);
    prefix[0] = prefix[1] = 0;
    assert(dns_tcp_frame_length(prefix, &length) != 0);
    prefix[0] = 0xff; prefix[1] = 0xff;
    assert(dns_tcp_frame_length(prefix, &length) == 0 && length == 65535);

    const uint8_t message[] = {0xab, 0xcd, 0x00, 0x01};
    uint8_t framed[16] = {0};
    size_t written = 0;
    assert(dns_tcp_frame_write(framed, sizeof(framed), message, sizeof(message), &written) == 0);
    assert(written == 6 && framed[0] == 0 && framed[1] == 4);
    assert(memcmp(framed + 2, message, sizeof(message)) == 0);
    assert(dns_tcp_frame_write(framed, 5, message, sizeof(message), &written) != 0);

    struct in_addr source, destination, other;
    assert(inet_pton(AF_INET, "127.0.0.1", &source) == 1);
    assert(inet_pton(AF_INET, "127.10.39.16", &destination) == 1);
    assert(inet_pton(AF_INET, "127.0.0.2", &other) == 1);
    /* Mock entries from one inet_diag DUMP reply, including unrelated and
     * connected sockets. This is the same input shape the netlink parser emits. */
    struct dns_udp_diag_socket dump[] = {
        {.uid=22, .local_address=source, .local_port=40124},
        {.uid=10123, .local_address={.s_addr=htonl(INADDR_ANY)}, .local_port=40123},
        {.uid=44, .local_address=other, .local_port=40123},
        {.uid=55, .local_address=source, .local_port=40123,
         .remote_address=destination, .remote_port=53},
    };
    uint32_t owner = 0;
    assert(dns_forwarder_udp_uid(dump, 4, source, 40123, destination, 53, &owner) == 0);
    assert(owner == 55);
    dump[3].remote_port = 54;
    assert(dns_forwarder_udp_uid(dump, 4, source, 40123, destination, 53, &owner) == 0);
    assert(owner == 10123);
    dump[3].remote_port = 53;
    dump[2].local_address = source;
    dump[3].remote_port = 54;
    assert(dns_forwarder_udp_uid(dump, 4, source, 40123, destination, 53, &owner) != 0);
    dump[2].local_address = other;
    assert(dns_forwarder_udp_uid(dump, 4, source, 40123, other, 53, &owner) == 0);

    char address[16];
    dns_forwarder_address(0x123456, address);
    assert(strcmp(address, "127.28.52.86") == 0);
    dns_forwarder_address(110001, address);
    assert(strcmp(address, "127.11.173.177") == 0);
    char other_user[16];
    dns_forwarder_address(210001, other_user);
    assert(strcmp(other_user, "127.13.52.81") == 0);
    assert(strcmp(address, other_user) != 0);
    puts("PASS: tuple and UDP dump attribution, token bucket, DNS framing and UID addresses");
    return 0;
}
