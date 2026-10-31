#include "transport.h"
#include "common.h"
#include "network.h"

#include <stdlib.h>
#include <string.h>

#define MAX_FLOWS 16
#define MAX_PORTS 8
#define QUEUE_CAP 1024

typedef struct {
    uint32_t seq;
    uint16_t len;
    int sent;
    int tries;
    uint64_t sent_us;
    uint8_t data[MAX_SEGMENT_PAYLOAD];
} Segment;

typedef struct {
    uint16_t port;
    tp_data_fn on_data;
    tp_error_fn on_error;
} Port;

typedef struct {
    int used, failed;
    uint16_t local_port, remote_addr, remote_port;
    uint32_t base, next_seq, expected, unrel_seq, unrel_last;
    int unrel_seen;
    Segment *queue[QUEUE_CAP];
    size_t head, count;
    uint64_t timer_start;
} Flow;

static TpConfig g_cfg = { 4, 200, 10 };
static Port g_ports[MAX_PORTS];
static int g_nports;
static Flow g_flows[MAX_FLOWS];
static TpStats g_stats;

void tp_encode_header(uint8_t *out, const TpHeader *h)
{
    put16(out, h->src_port);
    put16(out + 2, h->dst_port);
    put32(out + 4, h->seq);
    put32(out + 8, h->ack);
    out[12] = h->flags;
}

int tp_decode_header(const uint8_t *in, size_t len, TpHeader *h)
{
    if (len < TP_HEADER_SIZE)
        return -1;
    h->src_port = get16(in);
    h->dst_port = get16(in + 2);
    h->seq = get32(in + 4);
    h->ack = get32(in + 8);
    h->flags = in[12];
    if (len - TP_HEADER_SIZE > MAX_SEGMENT_PAYLOAD)
        return -1;
    if (!(h->flags & (TP_FLAG_DATA | TP_FLAG_ACK)))
        return -1;
    return 0;
}

int tp_init(const TpConfig *cfg)
{
    g_cfg = *cfg;
    if (g_cfg.window < 1)
        g_cfg.window = 1;
    return 0;
}

int tp_bind(uint16_t port, tp_data_fn on_data, tp_error_fn on_error)
{
    if (g_nports >= MAX_PORTS)
        return -1;
    for (int i = 0; i < g_nports; i++)
        if (g_ports[i].port == port)
            return -1;
    g_ports[g_nports].port = port;
    g_ports[g_nports].on_data = on_data;
    g_ports[g_nports].on_error = on_error;
    g_nports++;
    return 0;
}

static Port *port_find(uint16_t port)
{
    for (int i = 0; i < g_nports; i++)
        if (g_ports[i].port == port)
            return &g_ports[i];
    return NULL;
}

static Flow *flow_get(uint16_t lport, uint16_t raddr, uint16_t rport, int create)
{
    Flow *free_slot = NULL;
    for (int i = 0; i < MAX_FLOWS; i++) {
        Flow *f = &g_flows[i];
        if (f->used && f->local_port == lport && f->remote_addr == raddr && f->remote_port == rport)
            return f;
        if (!f->used && !free_slot)
            free_slot = f;
    }
    if (!create || !free_slot)
        return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = 1;
    free_slot->local_port = lport;
    free_slot->remote_addr = raddr;
    free_slot->remote_port = rport;
    return free_slot;
}

static Segment *q_at(const Flow *f, size_t i)
{
    return f->queue[(f->head + i) % QUEUE_CAP];
}

static size_t inflight(const Flow *f)
{
    size_t n = 0;
    while (n < f->count && q_at(f, n)->sent)
        n++;
    return n;
}

static const char *flag_str(uint8_t flags, char *buf)
{
    buf[0] = '\0';
    if (flags & TP_FLAG_DATA)
        strcat(buf, "DATA");
    if (flags & TP_FLAG_ACK)
        strcat(buf, buf[0] ? "|ACK" : "ACK");
    if (flags & TP_FLAG_REL)
        strcat(buf, "|REL");
    return buf;
}

static void emit(const Flow *f, uint8_t flags, uint32_t seq, uint32_t ack, const uint8_t *payload, size_t len)
{
    uint8_t buf[TP_HEADER_SIZE + MAX_SEGMENT_PAYLOAD];
    TpHeader h = { f->local_port, f->remote_port, seq, ack, flags };
    tp_encode_header(buf, &h);
    if (len)
        memcpy(buf + TP_HEADER_SIZE, payload, len);
    char fs[24];
    trace("TRP", "SEND", "sport=%u dport=%u seq=%u ack=%u flags=%s  cabecalho %d B + carga %zu B",
          h.src_port, h.dst_port, seq, ack, flag_str(flags, fs), TP_HEADER_SIZE, len);
    net_send(f->remote_addr, buf, TP_HEADER_SIZE + len);
}

static void transmit(Flow *f, Segment *s, uint64_t now, int retransmission)
{
    emit(f, TP_FLAG_DATA | TP_FLAG_REL, s->seq, 0, s->data, s->len);
    g_stats.segments_tx++;
    if (retransmission) {
        g_stats.retransmissions++;
        s->tries++;
    }
    s->sent = 1;
    s->sent_us = now;
}

static void try_send(Flow *f)
{
    size_t limit = f->count < (size_t)g_cfg.window ? f->count : (size_t)g_cfg.window;
    int was_idle = inflight(f) == 0;
    uint64_t now = now_us();
    for (size_t i = 0; i < limit; i++) {
        Segment *s = q_at(f, i);
        if (!s->sent)
            transmit(f, s, now, 0);
    }
    if (was_idle && inflight(f) > 0)
        f->timer_start = now;
}

static void fail_flow(Flow *f)
{
    while (f->count > 0) {
        free(f->queue[f->head]);
        f->head = (f->head + 1) % QUEUE_CAP;
        f->count--;
    }
    f->failed = 1;
    g_stats.failed_flows++;
    Port *p = port_find(f->local_port);
    if (p && p->on_error)
        p->on_error(f->remote_addr, f->remote_port, TP_ERR_TIMEOUT);
}

int tp_send(uint16_t local_port, uint16_t dst_addr, uint16_t dst_port,
            const uint8_t *data, size_t len, int reliable)
{
    if (len > MAX_SEGMENT_PAYLOAD || !port_find(local_port))
        return -2;
    Flow *f = flow_get(local_port, dst_addr, dst_port, 1);
    if (!f)
        return -2;
    if (f->failed)
        return -3;
    if (!reliable) {
        emit(f, TP_FLAG_DATA, f->unrel_seq++, 0, data, len);
        g_stats.unrel_tx++;
        return 0;
    }
    if (f->count >= QUEUE_CAP)
        return -1;
    Segment *s = calloc(1, sizeof *s);
    if (!s)
        return -2;
    s->seq = f->next_seq++;
    s->len = (uint16_t)len;
    memcpy(s->data, data, len);
    f->queue[(f->head + f->count) % QUEUE_CAP] = s;
    f->count++;
    try_send(f);
    return 0;
}

size_t tp_pending(void)
{
    size_t n = 0;
    for (int i = 0; i < MAX_FLOWS; i++)
        if (g_flows[i].used && !g_flows[i].failed)
            n += g_flows[i].count;
    return n;
}

static void handle_ack(const TpHeader *h, uint16_t src_addr)
{
    Flow *f = flow_get(h->dst_port, src_addr, h->src_port, 0);
    if (!f) {
        g_stats.bad_acks++;
        return;
    }
    g_stats.acks_rx++;
    uint64_t now = now_us();
    if (h->ack <= f->base) {
        g_stats.dup_acks_rx++;
        return;
    }
    uint32_t advance = h->ack - f->base;
    if (advance > inflight(f)) {
        g_stats.bad_acks++;
        return;
    }
    Segment *last = q_at(f, advance - 1);
    if (last->tries == 0 && now >= last->sent_us) {
        g_stats.rtt_samples++;
        g_stats.rtt_sum_us += now - last->sent_us;
    }
    for (uint32_t i = 0; i < advance; i++) {
        free(f->queue[f->head]);
        f->head = (f->head + 1) % QUEUE_CAP;
        f->count--;
    }
    f->base = h->ack;
    f->timer_start = now;
    try_send(f);
}

static void send_ack(const Flow *f)
{
    emit(f, TP_FLAG_ACK, 0, f->expected, NULL, 0);
    g_stats.acks_tx++;
}

static void handle_data(const TpHeader *h, uint16_t src_addr, const uint8_t *payload, size_t plen)
{
    Port *p = port_find(h->dst_port);
    if (!p) {
        g_stats.rx_no_port++;
        return;
    }
    Flow *f = flow_get(h->dst_port, src_addr, h->src_port, 1);
    if (!f) {
        g_stats.rx_flow_table_full++;
        return;
    }
    if (h->flags & TP_FLAG_REL) {
        if (h->seq == f->expected) {
            f->expected++;
            g_stats.delivered++;
            p->on_data(src_addr, h->src_port, payload, plen);
        } else if (h->seq < f->expected) {
            g_stats.dup_rx++;
        } else {
            g_stats.out_of_order_dropped++;
        }
        send_ack(f);
        return;
    }
    if (!f->unrel_seen) {
        f->unrel_seen = 1;
        f->unrel_last = h->seq;
    } else if (h->seq > f->unrel_last) {
        g_stats.unrel_gaps += h->seq - f->unrel_last - 1;
        f->unrel_last = h->seq;
    } else {
        g_stats.unrel_reordered++;
    }
    g_stats.delivered++;
    p->on_data(src_addr, h->src_port, payload, plen);
}

void tp_input(uint16_t src_addr, const uint8_t *segment, size_t len)
{
    TpHeader h;
    if (tp_decode_header(segment, len, &h) < 0) {
        g_stats.rx_bad_header++;
        trace("TRP", "RECV", "segmento descartado: cabecalho invalido");
        return;
    }
    char fs[24];
    trace("TRP", "RECV", "sport=%u dport=%u seq=%u ack=%u flags=%s  carga %zu B",
          h.src_port, h.dst_port, h.seq, h.ack, flag_str(h.flags, fs), len - TP_HEADER_SIZE);
    if (h.flags & TP_FLAG_ACK)
        handle_ack(&h, src_addr);
    else
        handle_data(&h, src_addr, segment + TP_HEADER_SIZE, len - TP_HEADER_SIZE);
}

void tp_tick(uint64_t now)
{
    uint64_t rto = (uint64_t)g_cfg.rto_ms * 1000ull;
    for (int i = 0; i < MAX_FLOWS; i++) {
        Flow *f = &g_flows[i];
        if (!f->used || f->failed || f->count == 0)
            continue;
        size_t n = inflight(f);
        if (n == 0 || now < f->timer_start || now - f->timer_start < rto)
            continue;
        if (q_at(f, 0)->tries >= g_cfg.max_retries) {
            fail_flow(f);
            continue;
        }
        g_stats.timeouts++;
        trace("TRP", "RTO", "temporizador expirou: retransmitindo %zu segmento(s) a partir de seq=%u", n, f->base);
        uint64_t t = now_us();
        for (size_t k = 0; k < n; k++)
            transmit(f, q_at(f, k), t, 1);
        f->timer_start = t;
    }
}

uint64_t tp_next_deadline(void)
{
    uint64_t best = 0;
    uint64_t rto = (uint64_t)g_cfg.rto_ms * 1000ull;
    for (int i = 0; i < MAX_FLOWS; i++) {
        const Flow *f = &g_flows[i];
        if (!f->used || f->failed || f->count == 0 || inflight(f) == 0)
            continue;
        uint64_t due = f->timer_start + rto;
        if (best == 0 || due < best)
            best = due;
    }
    return best;
}

const TpStats *tp_stats(void) { return &g_stats; }

void tp_print_stats(FILE *out)
{
    const TpStats *s = &g_stats;
    fprintf(out, "STAT tp.segments_tx=%llu\n", (unsigned long long)s->segments_tx);
    fprintf(out, "STAT tp.retransmissions=%llu\n", (unsigned long long)s->retransmissions);
    fprintf(out, "STAT tp.timeouts=%llu\n", (unsigned long long)s->timeouts);
    fprintf(out, "STAT tp.unrel_tx=%llu\n", (unsigned long long)s->unrel_tx);
    fprintf(out, "STAT tp.acks_tx=%llu\n", (unsigned long long)s->acks_tx);
    fprintf(out, "STAT tp.acks_rx=%llu\n", (unsigned long long)s->acks_rx);
    fprintf(out, "STAT tp.dup_acks_rx=%llu\n", (unsigned long long)s->dup_acks_rx);
    fprintf(out, "STAT tp.bad_acks=%llu\n", (unsigned long long)s->bad_acks);
    fprintf(out, "STAT tp.delivered=%llu\n", (unsigned long long)s->delivered);
    fprintf(out, "STAT tp.dup_rx=%llu\n", (unsigned long long)s->dup_rx);
    fprintf(out, "STAT tp.out_of_order_dropped=%llu\n", (unsigned long long)s->out_of_order_dropped);
    fprintf(out, "STAT tp.unrel_gaps=%llu\n", (unsigned long long)s->unrel_gaps);
    fprintf(out, "STAT tp.unrel_reordered=%llu\n", (unsigned long long)s->unrel_reordered);
    fprintf(out, "STAT tp.rtt_samples=%llu\n", (unsigned long long)s->rtt_samples);
    fprintf(out, "STAT tp.rtt_sum_us=%llu\n", (unsigned long long)s->rtt_sum_us);
    fprintf(out, "STAT tp.rx_bad_header=%llu\n", (unsigned long long)s->rx_bad_header);
    fprintf(out, "STAT tp.rx_no_port=%llu\n", (unsigned long long)s->rx_no_port);
    fprintf(out, "STAT tp.rx_flow_table_full=%llu\n", (unsigned long long)s->rx_flow_table_full);
    fprintf(out, "STAT tp.failed_flows=%llu\n", (unsigned long long)s->failed_flows);
}
