#include "dns_forwarder_core.h"

#include <stdio.h>
#include <string.h>

int dns_forwarder_uid_allowed(dns_uid_lookup_fn lookup, void *context,
                              const void *peer, uint32_t expected_uid) {
    uint32_t actual_uid = UINT32_MAX;
    return lookup != NULL && lookup(context, peer, &actual_uid) == 0 &&
           actual_uid == expected_uid;
}

void dns_token_bucket_init(struct dns_token_bucket *bucket, double capacity,
                           double tokens_per_second, uint64_t now_ns) {
    if (bucket == NULL) return;
    bucket->capacity = capacity > 0 ? capacity : 1;
    bucket->tokens_per_second = tokens_per_second > 0 ? tokens_per_second : 1;
    bucket->tokens = bucket->capacity;
    bucket->last_ns = now_ns;
}

int dns_token_bucket_take(struct dns_token_bucket *bucket, uint64_t now_ns) {
    if (bucket == NULL) return 0;
    if (now_ns > bucket->last_ns) {
        double elapsed = (double)(now_ns - bucket->last_ns) / 1000000000.0;
        bucket->tokens += elapsed * bucket->tokens_per_second;
        if (bucket->tokens > bucket->capacity) bucket->tokens = bucket->capacity;
        bucket->last_ns = now_ns;
    }
    if (bucket->tokens < 1.0) return 0;
    bucket->tokens -= 1.0;
    return 1;
}

int dns_tcp_frame_length(const uint8_t prefix[2], size_t *frame_length) {
    if (prefix == NULL || frame_length == NULL) return -1;
    size_t length = ((size_t)prefix[0] << 8) | prefix[1];
    if (length == 0 || length > DNS_FORWARDER_MAX_PACKET) return -1;
    *frame_length = length;
    return 0;
}

int dns_tcp_frame_write(uint8_t *output, size_t capacity, const uint8_t *message,
                        size_t message_length, size_t *written) {
    if (output == NULL || message == NULL || written == NULL ||
        message_length == 0 || message_length > DNS_FORWARDER_MAX_PACKET ||
        capacity < message_length + DNS_FORWARDER_TCP_PREFIX) return -1;
    output[0] = (uint8_t)(message_length >> 8);
    output[1] = (uint8_t)message_length;
    memcpy(output + DNS_FORWARDER_TCP_PREFIX, message, message_length);
    *written = message_length + DNS_FORWARDER_TCP_PREFIX;
    return 0;
}

void dns_forwarder_address(uint32_t uid, char output[16]) {
    if (output == NULL) return;
    snprintf(output, 16, "127.10.%u.%u", (uid >> 8) & 0xff, uid & 0xff);
}
