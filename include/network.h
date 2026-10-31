#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define NET_HEADER_SIZE 9
#define NET_VERSION 1
#define NET_PROTO_TRANSPORT 1

typedef struct {
    uint8_t version, ttl, protocol;
    uint16_t src, dst, payload_len;
} NetHeader;

typedef struct {
    uint64_t packets_tx, delivered, forwarded, ttl_expired, no_route, bad_header, bad_protocol;
} NetStats;

typedef void (*net_rx_fn)(uint16_t src, const uint8_t *segment, size_t len);

int net_init(uint16_t local_addr, uint8_t ttl);
/* Static routing: packets for dst leave through the neighbor next_hop. */
int net_add_route(uint16_t dst, uint16_t next_hop);
void net_set_upper(net_rx_fn fn);
/* Wraps a transport segment in a network header and hands it to the link layer. */
int net_send(uint16_t dst, const uint8_t *segment, size_t len);
/* Entry point for packets coming from the link layer: deliver locally or forward. */
void net_receive(uint16_t from, const uint8_t *packet, size_t len);
const NetStats *net_stats(void);
void net_print_stats(FILE *out);

void net_encode_header(uint8_t *out, const NetHeader *h);
int net_decode_header(const uint8_t *in, size_t len, NetHeader *h);

#endif
