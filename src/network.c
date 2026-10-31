#include "network.h"
#include "common.h"
#include "link.h"

#include <string.h>

#define MAX_ROUTES 64

typedef struct {
    uint16_t dst, next_hop;
} Route;

static uint16_t g_local;
static uint8_t g_ttl;
static Route g_routes[MAX_ROUTES];
static int g_nroutes;
static net_rx_fn g_upper;
static NetStats g_stats;

void net_encode_header(uint8_t *out, const NetHeader *h)
{
    out[0] = h->version;
    out[1] = h->ttl;
    put16(out + 2, h->src);
    put16(out + 4, h->dst);
    out[6] = h->protocol;
    put16(out + 7, h->payload_len);
}

int net_decode_header(const uint8_t *in, size_t len, NetHeader *h)
{
    if (len < NET_HEADER_SIZE)
        return -1;
    h->version = in[0];
    h->ttl = in[1];
    h->src = get16(in + 2);
    h->dst = get16(in + 4);
    h->protocol = in[6];
    h->payload_len = get16(in + 7);
    if (h->version != NET_VERSION || h->payload_len != len - NET_HEADER_SIZE)
        return -1;
    return 0;
}

int net_init(uint16_t local_addr, uint8_t ttl)
{
    g_local = local_addr;
    g_ttl = ttl;
    return 0;
}

int net_add_route(uint16_t dst, uint16_t next_hop)
{
    for (int i = 0; i < g_nroutes; i++) {
        if (g_routes[i].dst == dst) {
            g_routes[i].next_hop = next_hop;
            return 0;
        }
    }
    if (g_nroutes >= MAX_ROUTES)
        return -1;
    g_routes[g_nroutes].dst = dst;
    g_routes[g_nroutes].next_hop = next_hop;
    g_nroutes++;
    return 0;
}

static int lookup(uint16_t dst, uint16_t *next_hop)
{
    for (int i = 0; i < g_nroutes; i++) {
        if (g_routes[i].dst == dst) {
            *next_hop = g_routes[i].next_hop;
            return 0;
        }
    }
    return -1;
}

void net_set_upper(net_rx_fn fn) { g_upper = fn; }
const NetStats *net_stats(void) { return &g_stats; }

int net_send(uint16_t dst, const uint8_t *segment, size_t len)
{
    if (len > MAX_FRAME - LINK_HEADER_SIZE - NET_HEADER_SIZE)
        return -1;
    uint16_t next;
    if (lookup(dst, &next) < 0) {
        g_stats.no_route++;
        return -1;
    }
    uint8_t pkt[MAX_FRAME];
    NetHeader h = { NET_VERSION, g_ttl, NET_PROTO_TRANSPORT, g_local, dst, (uint16_t)len };
    net_encode_header(pkt, &h);
    memcpy(pkt + NET_HEADER_SIZE, segment, len);
    g_stats.packets_tx++;
    trace("NET", "SEND", "src=%u dst=%u ttl=%u proto=%u len=%zu  cabecalho %d B + segmento %zu B, proximo salto %u",
          g_local, dst, g_ttl, NET_PROTO_TRANSPORT, len, NET_HEADER_SIZE, len, next);
    return link_send(next, pkt, NET_HEADER_SIZE + len);
}

void net_receive(uint16_t from, const uint8_t *packet, size_t len)
{
    (void)from;
    NetHeader h;
    if (len > MAX_FRAME - LINK_HEADER_SIZE || net_decode_header(packet, len, &h) < 0) {
        g_stats.bad_header++;
        trace("NET", "RECV", "pacote descartado: cabecalho invalido");
        return;
    }
    if (h.dst == g_local) {
        if (h.protocol != NET_PROTO_TRANSPORT) {
            g_stats.bad_protocol++;
            return;
        }
        g_stats.delivered++;
        trace("NET", "RECV", "src=%u dst=%u ttl=%u proto=%u -> segmento de %u B para o transporte",
              h.src, h.dst, h.ttl, h.protocol, h.payload_len);
        if (g_upper)
            g_upper(h.src, packet + NET_HEADER_SIZE, h.payload_len);
        return;
    }
    if (h.ttl <= 1) {
        g_stats.ttl_expired++;
        trace("NET", "DROP", "src=%u dst=%u: TTL esgotado", h.src, h.dst);
        return;
    }
    uint16_t next;
    if (lookup(h.dst, &next) < 0) {
        g_stats.no_route++;
        return;
    }
    uint8_t copy[MAX_FRAME];
    memcpy(copy, packet, len);
    copy[1] = (uint8_t)(h.ttl - 1);
    g_stats.forwarded++;
    trace("NET", "FWD", "src=%u dst=%u ttl=%u->%u via %u", h.src, h.dst, h.ttl, h.ttl - 1, next);
    link_send(next, copy, len);
}

void net_print_stats(FILE *out)
{
    const NetStats *s = &g_stats;
    fprintf(out, "STAT net.packets_tx=%llu\n", (unsigned long long)s->packets_tx);
    fprintf(out, "STAT net.delivered=%llu\n", (unsigned long long)s->delivered);
    fprintf(out, "STAT net.forwarded=%llu\n", (unsigned long long)s->forwarded);
    fprintf(out, "STAT net.ttl_expired=%llu\n", (unsigned long long)s->ttl_expired);
    fprintf(out, "STAT net.no_route=%llu\n", (unsigned long long)s->no_route);
    fprintf(out, "STAT net.bad_header=%llu\n", (unsigned long long)s->bad_header);
    fprintf(out, "STAT net.bad_protocol=%llu\n", (unsigned long long)s->bad_protocol);
}
