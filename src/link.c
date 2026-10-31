#include "link.h"
#include "common.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_NEIGHBORS 16
#define MAX_DELAYED 2048

typedef struct {
    uint16_t addr;
    struct sockaddr_in sa;
} Neighbor;

typedef struct {
    uint64_t due;
    size_t len;
    struct sockaddr_in sa;
    uint8_t data[MAX_FRAME];
} Delayed;

static int g_fd = -1;
static LinkConfig g_cfg;
static Neighbor g_nb[MAX_NEIGHBORS];
static int g_nnb;
static Delayed *g_delayed;
static size_t g_ndelayed;
static link_rx_fn g_upper;
static LinkStats g_stats;
static uint64_t g_rng = 88172645463325252ull;

static uint64_t rng_next(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 2685821657736338717ull;
}

static double rng_unit(void)
{
    return (double)(rng_next() >> 11) / 9007199254740992.0;
}

static uint16_t crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint16_t crc16(const uint8_t *data, size_t len)
{
    return crc16_update(0xFFFF, data, len);
}

size_t link_encode_frame(uint8_t *out, const uint8_t *packet, size_t len)
{
    put16(out, (uint16_t)len);
    out[2] = out[3] = 0;
    memcpy(out + LINK_HEADER_SIZE, packet, len);
    put16(out + 2, crc16(out, LINK_HEADER_SIZE + len));
    return LINK_HEADER_SIZE + len;
}

int link_decode_frame(const uint8_t *frame, size_t frame_len, int verify,
                      const uint8_t **packet, size_t *len)
{
    if (frame_len < LINK_HEADER_SIZE || frame_len > MAX_FRAME || get16(frame) != frame_len - LINK_HEADER_SIZE)
        return LINK_ERR_LENGTH;
    if (verify) {
        static const uint8_t zeros[2] = { 0, 0 };
        uint16_t crc = crc16_update(0xFFFF, frame, 2);
        crc = crc16_update(crc, zeros, 2);
        crc = crc16_update(crc, frame + LINK_HEADER_SIZE, frame_len - LINK_HEADER_SIZE);
        if (crc != get16(frame + 2))
            return LINK_ERR_CHECKSUM;
    }
    *packet = frame + LINK_HEADER_SIZE;
    *len = frame_len - LINK_HEADER_SIZE;
    return 0;
}

int link_init(const LinkConfig *cfg, uint16_t udp_port)
{
    g_cfg = *cfg;
    g_rng = cfg->seed ? cfg->seed * 0x9E3779B97F4A7C15ull : g_rng;
    if (g_rng == 0)
        g_rng = 1;
    for (int i = 0; i < 16; i++)
        rng_next();

    g_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_fd < 0)
        return -1;
    int buf = 4 << 20;
    setsockopt(g_fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    setsockopt(g_fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(udp_port);
    if (bind(g_fd, (struct sockaddr *)&sa, sizeof sa) < 0)
        return -1;
    fcntl(g_fd, F_SETFL, fcntl(g_fd, F_GETFL, 0) | O_NONBLOCK);

    g_delayed = malloc(MAX_DELAYED * sizeof(Delayed));
    return g_delayed ? 0 : -1;
}

int link_add_neighbor(uint16_t addr, const char *ip, uint16_t udp_port)
{
    if (g_nnb >= MAX_NEIGHBORS)
        return -1;
    Neighbor *n = &g_nb[g_nnb];
    memset(n, 0, sizeof *n);
    n->addr = addr;
    n->sa.sin_family = AF_INET;
    n->sa.sin_port = htons(udp_port);
    if (inet_pton(AF_INET, ip, &n->sa.sin_addr) != 1)
        return -1;
    g_nnb++;
    return 0;
}

int link_fd(void) { return g_fd; }
void link_set_upper(link_rx_fn fn) { g_upper = fn; }
const LinkStats *link_stats(void) { return &g_stats; }
size_t link_queued(void) { return g_ndelayed; }

static Neighbor *find_by_addr(uint16_t addr)
{
    for (int i = 0; i < g_nnb; i++)
        if (g_nb[i].addr == addr)
            return &g_nb[i];
    return NULL;
}

static Neighbor *find_by_sockaddr(const struct sockaddr_in *sa)
{
    for (int i = 0; i < g_nnb; i++)
        if (g_nb[i].sa.sin_addr.s_addr == sa->sin_addr.s_addr && g_nb[i].sa.sin_port == sa->sin_port)
            return &g_nb[i];
    return NULL;
}

static void raw_send(const uint8_t *data, size_t len, const struct sockaddr_in *to)
{
    if (sendto(g_fd, data, len, 0, (const struct sockaddr *)to, sizeof *to) < 0)
        g_stats.tx_errors++;
}

int link_send(uint16_t neighbor, const uint8_t *packet, size_t len)
{
    Neighbor *n = find_by_addr(neighbor);
    if (!n || len == 0 || len > MAX_FRAME - LINK_HEADER_SIZE)
        return -1;

    uint8_t frame[MAX_FRAME];
    size_t flen = link_encode_frame(frame, packet, len);
    g_stats.frames_tx++;
    g_stats.bytes_tx += flen;
    trace("LNK", "SEND", "len=%zu crc=0x%04x  cabecalho %d B + carga %zu B = frame %zu B -> vizinho %u",
          len, get16(frame + 2), LINK_HEADER_SIZE, len, flen, neighbor);

    if (rng_unit() < g_cfg.loss) {
        g_stats.frames_lost_sim++;
        trace("LNK", "DROP", "frame descartado pela simulacao de perda");
        return 0;
    }
    if (rng_unit() < g_cfg.corrupt) {
        int flips = 1 + (int)(rng_next() % 3);
        for (int i = 0; i < flips; i++) {
            size_t bit = (size_t)(rng_next() % (flen * 8));
            frame[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        }
        g_stats.frames_corrupted_sim++;
        trace("LNK", "CORR", "%d bit(s) invertido(s) pela simulacao de corrupcao", flips);
    }

    double delay = g_cfg.delay_ms;
    if (g_cfg.jitter_ms > 0)
        delay += rng_unit() * g_cfg.jitter_ms;
    if (delay <= 0) {
        raw_send(frame, flen, &n->sa);
        return 0;
    }
    if (g_ndelayed >= MAX_DELAYED) {
        g_stats.queue_overflow++;
        return 0;
    }
    Delayed *d = &g_delayed[g_ndelayed++];
    d->due = now_us() + (uint64_t)(delay * 1000.0);
    d->len = flen;
    d->sa = n->sa;
    memcpy(d->data, frame, flen);
    g_stats.frames_delayed++;
    trace("LNK", "DELAY", "frame retido por %.1f ms", delay);
    return 0;
}

void link_tick(uint64_t now)
{
    size_t keep = 0;
    for (size_t i = 0; i < g_ndelayed; i++) {
        if (g_delayed[i].due <= now) {
            raw_send(g_delayed[i].data, g_delayed[i].len, &g_delayed[i].sa);
        } else {
            if (keep != i)
                memcpy(&g_delayed[keep], &g_delayed[i], sizeof(Delayed));
            keep++;
        }
    }
    g_ndelayed = keep;
}

uint64_t link_next_deadline(void)
{
    uint64_t best = 0;
    for (size_t i = 0; i < g_ndelayed; i++)
        if (best == 0 || g_delayed[i].due < best)
            best = g_delayed[i].due;
    return best;
}

void link_on_readable(void)
{
    uint8_t buf[MAX_FRAME + 64];
    for (int budget = 512; budget > 0; budget--) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(g_fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0)
            return;
        Neighbor *nb = find_by_sockaddr(&from);
        if (!nb) {
            g_stats.rx_unknown_neighbor++;
            continue;
        }
        g_stats.frames_rx++;
        g_stats.bytes_rx += (uint64_t)n;
        const uint8_t *pkt;
        size_t plen;
        int rc = link_decode_frame(buf, (size_t)n, g_cfg.verify_checksum, &pkt, &plen);
        if (rc == LINK_ERR_CHECKSUM) {
            g_stats.rx_bad_checksum++;
            trace("LNK", "RECV", "frame de %zd B descartado: CRC-16 invalido", n);
            continue;
        }
        if (rc != 0) {
            g_stats.rx_bad_length++;
            trace("LNK", "RECV", "frame de %zd B descartado: tamanho inconsistente", n);
            continue;
        }
        trace("LNK", "RECV", "frame %zd B, CRC ok -> pacote de %zu B para a camada de rede", n, plen);
        if (g_upper)
            g_upper(nb->addr, pkt, plen);
    }
}

void link_print_stats(FILE *out)
{
    const LinkStats *s = &g_stats;
    fprintf(out, "STAT link.frames_tx=%llu\n", (unsigned long long)s->frames_tx);
    fprintf(out, "STAT link.bytes_tx=%llu\n", (unsigned long long)s->bytes_tx);
    fprintf(out, "STAT link.frames_lost_sim=%llu\n", (unsigned long long)s->frames_lost_sim);
    fprintf(out, "STAT link.frames_corrupted_sim=%llu\n", (unsigned long long)s->frames_corrupted_sim);
    fprintf(out, "STAT link.frames_delayed=%llu\n", (unsigned long long)s->frames_delayed);
    fprintf(out, "STAT link.queue_overflow=%llu\n", (unsigned long long)s->queue_overflow);
    fprintf(out, "STAT link.tx_errors=%llu\n", (unsigned long long)s->tx_errors);
    fprintf(out, "STAT link.frames_rx=%llu\n", (unsigned long long)s->frames_rx);
    fprintf(out, "STAT link.bytes_rx=%llu\n", (unsigned long long)s->bytes_rx);
    fprintf(out, "STAT link.rx_bad_checksum=%llu\n", (unsigned long long)s->rx_bad_checksum);
    fprintf(out, "STAT link.rx_bad_length=%llu\n", (unsigned long long)s->rx_bad_length);
    fprintf(out, "STAT link.rx_unknown_neighbor=%llu\n", (unsigned long long)s->rx_unknown_neighbor);
}
