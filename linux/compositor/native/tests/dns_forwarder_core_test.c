#include "../dns_forwarder_core.h"

#include <assert.h>
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

    char address[16];
    dns_forwarder_address(0x1234, address);
    assert(strcmp(address, "127.10.18.52") == 0);
    puts("PASS: uid check, token bucket, DNS TCP framing and uid-derived address");
    return 0;
}
