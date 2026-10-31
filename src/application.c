#include "application.h"
#include "common.h"
#include "transport.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PEERS 8
#define BENCH_MAX_TRACKED BENCH_MAX_MESSAGES
#define BENCH_MAX_PENDING 1024

typedef enum { S_NONE, S_HELLO_SENT, S_ESTABLISHED, S_CLOSED } SessionState;

typedef struct {
    int used;
    uint16_t peer;
    SessionState state;
    char name[33];
} Session;

typedef struct {
    uint64_t chat_tx, chat_rx, chat_rejected, chat_sessions, delivery_errors;
    uint64_t bench_sent, bench_send_errors, bench_elapsed_us;
    uint64_t bench_rx, bench_bytes, bench_unique, bench_dups, bench_in_order;
    uint64_t bench_skipped, bench_reordered, bench_bad_payload, bench_bad_format;
} AppStats;

typedef struct {
    int active, done, failed, reliable;
    uint16_t peer;
    uint32_t count, sent;
    size_t size;
    uint64_t interval_us, next_us, start_us;
} Bench;

static char g_name[33] = "user";
static Session g_sessions[MAX_PEERS];
static int g_current = -1;
static char g_pending_text[CHAT_MAX_TEXT + 1];
static int g_has_pending;
static int g_quitting;
static Bench g_bench;
static AppStats g_stats;
static uint8_t *g_seen;
static uint32_t g_next_id;

static uint8_t bench_pattern(uint32_t id, size_t i)
{
    return (uint8_t)((id ^ (id >> 8) ^ (id >> 16) ^ (id >> 24)) + i);
}

static void copy_name(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static Session *session_get(uint16_t peer, int create)
{
    Session *free_slot = NULL;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (g_sessions[i].used && g_sessions[i].peer == peer)
            return &g_sessions[i];
        if (!g_sessions[i].used && !free_slot)
            free_slot = &g_sessions[i];
    }
    if (!create || !free_slot)
        return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = 1;
    free_slot->peer = peer;
    return free_slot;
}

static const char *chat_type_name(uint8_t t)
{
    switch (t) {
    case CHAT_HELLO: return "HELLO";
    case CHAT_HELLO_ACK: return "HELLO_ACK";
    case CHAT_TEXT: return "TEXT";
    case CHAT_BYE: return "BYE";
    default: return "?";
    }
}

static int chat_send(uint16_t peer, uint8_t type, const char *text)
{
    uint8_t msg[CHAT_MAX_TEXT + 1];
    size_t n = text ? strlen(text) : 0;
    if (n > CHAT_MAX_TEXT)
        n = CHAT_MAX_TEXT;
    msg[0] = type;
    if (n)
        memcpy(msg + 1, text, n);
    trace("APP", "SEND", "chat %s \"%.*s\"  (1 B de tipo + %zu B de texto) -> no %u porta %d",
          chat_type_name(type), (int)n, text ? text : "", n, peer, CHAT_PORT);
    return tp_send(CHAT_PORT, peer, CHAT_PORT, msg, 1 + n, 1);
}

static size_t utf8_seq_len(const uint8_t *s, size_t n)
{
    size_t need = s[0] >= 0xC2 && s[0] <= 0xDF ? 2 : s[0] >= 0xE0 && s[0] <= 0xEF ? 3 : s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : 0;
    if (need == 0 || need > n)
        return 0;
    uint32_t cp = need == 2 ? s[0] & 0x1Fu : need == 3 ? s[0] & 0x0Fu : s[0] & 0x07u;
    for (size_t i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    uint32_t min = need == 2 ? 0x80u : need == 3 ? 0x800u : 0x10000u;
    if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu) || (cp >= 0x80u && cp <= 0x9Fu))
        return 0;
    return need;
}

static void sanitize(char *dst, const uint8_t *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (src[i] >= 32 && src[i] < 127) {
            dst[o++] = (char)src[i++];
        } else if (src[i] >= 0xC2) {
            size_t k = utf8_seq_len(src + i, n - i);
            if (k == 0) {
                dst[o++] = '?';
                i++;
            } else {
                memcpy(dst + o, src + i, k);
                o += k;
                i += k;
            }
        } else {
            dst[o++] = '?';
            i++;
        }
    }
    dst[o] = '\0';
}

void app_chat_on_data(uint16_t src, uint16_t src_port, const uint8_t *data, size_t len)
{
    (void)src_port;
    if (len < 1 || len - 1 > CHAT_MAX_TEXT)
        return;
    char text[CHAT_MAX_TEXT + 1];
    sanitize(text, data + 1, len - 1);
    trace("APP", "RECV", "chat %s \"%s\" de %u", chat_type_name(data[0]), text, src);

    Session *s;
    switch (data[0]) {
    case CHAT_HELLO:
        s = session_get(src, 1);
        if (!s)
            return;
        s->state = S_ESTABLISHED;
        copy_name(s->name, sizeof s->name, text);
        g_stats.chat_sessions++;
        if (g_current < 0)
            g_current = src;
        printf("[chat] sessao iniciada por %u (%s)\n", src, s->name);
        fflush(stdout);
        chat_send(src, CHAT_HELLO_ACK, g_name);
        break;
    case CHAT_HELLO_ACK:
        s = session_get(src, 0);
        if (!s || s->state != S_HELLO_SENT)
            return;
        s->state = S_ESTABLISHED;
        copy_name(s->name, sizeof s->name, text);
        g_stats.chat_sessions++;
        printf("[chat] sessao estabelecida com %u (%s)\n", src, s->name);
        fflush(stdout);
        if (g_has_pending) {
            g_has_pending = 0;
            if (chat_send(src, CHAT_TEXT, g_pending_text) == 0)
                g_stats.chat_tx++;
        }
        break;
    case CHAT_TEXT:
        s = session_get(src, 0);
        if (!s || s->state != S_ESTABLISHED) {
            g_stats.chat_rejected++;
            return;
        }
        g_stats.chat_rx++;
        printf("[%s@%u] %s\n", s->name, src, text);
        fflush(stdout);
        break;
    case CHAT_BYE:
        s = session_get(src, 0);
        if (!s || s->state != S_ESTABLISHED)
            return;
        s->state = S_CLOSED;
        printf("[chat] sessao encerrada por %u\n", src);
        fflush(stdout);
        break;
    default:
        break;
    }
}

void app_chat_connect(uint16_t peer)
{
    Session *s = session_get(peer, 1);
    if (!s)
        return;
    s->state = S_HELLO_SENT;
    g_current = peer;
    chat_send(peer, CHAT_HELLO, g_name);
}

void app_chat_queue_text(const char *text)
{
    snprintf(g_pending_text, sizeof g_pending_text, "%s", text);
    g_has_pending = 1;
}

int app_chat_finished(void)
{
    if (!g_has_pending)
        return 1;
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_sessions[i].used && g_sessions[i].state == S_HELLO_SENT)
            return 0;
    return 1;
}

int app_chat_line(const char *line)
{
    if (strcmp(line, "/quit") == 0) {
        for (int i = 0; i < MAX_PEERS; i++) {
            if (g_sessions[i].used && g_sessions[i].state == S_ESTABLISHED) {
                chat_send(g_sessions[i].peer, CHAT_BYE, NULL);
                g_sessions[i].state = S_CLOSED;
            }
        }
        g_quitting = 1;
        return 1;
    }
    Session *s = g_current >= 0 ? session_get((uint16_t)g_current, 0) : NULL;
    if (!s || s->state != S_ESTABLISHED) {
        printf("[chat] nenhuma sessao ativa\n");
        fflush(stdout);
        return 0;
    }
    if (chat_send(s->peer, CHAT_TEXT, line) == 0)
        g_stats.chat_tx++;
    return 0;
}

static void on_error(uint16_t dst_addr, uint16_t dst_port, int err)
{
    (void)err;
    g_stats.delivery_errors++;
    if (dst_port == CHAT_PORT) {
        Session *s = session_get(dst_addr, 0);
        if (s && s->state == S_HELLO_SENT)
            s->state = S_CLOSED;
    }
    if (dst_port == BENCH_PORT && g_bench.active && g_bench.peer == dst_addr) {
        g_bench.failed = 1;
        g_bench.done = 1;
    }
    printf("[app] falha de entrega para %u porta %u: limite de retransmissoes atingido\n", dst_addr, dst_port);
    fflush(stdout);
}

void app_bench_on_data(uint16_t src, uint16_t src_port, const uint8_t *data, size_t len)
{
    (void)src;
    (void)src_port;
    if (len < BENCH_HEADER_SIZE || data[0] != BENCH_DATA) {
        g_stats.bench_bad_format++;
        return;
    }
    uint32_t id = get32(data + 1);
    for (size_t i = 0; i < len - BENCH_HEADER_SIZE; i++) {
        if (data[BENCH_HEADER_SIZE + i] != bench_pattern(id, i)) {
            g_stats.bench_bad_payload++;
            return;
        }
    }
    trace("APP", "RECV", "bench id=%u tamanho=%zu B", id, len);
    g_stats.bench_rx++;
    g_stats.bench_bytes += len;
    if (id < BENCH_MAX_TRACKED) {
        if (g_seen[id >> 3] & (1u << (id & 7))) {
            g_stats.bench_dups++;
        } else {
            g_seen[id >> 3] |= (uint8_t)(1u << (id & 7));
            g_stats.bench_unique++;
        }
    }
    if (id == g_next_id) {
        g_stats.bench_in_order++;
        g_next_id++;
    } else if (id > g_next_id) {
        g_stats.bench_skipped += id - g_next_id;
        g_next_id = id + 1;
    } else {
        g_stats.bench_reordered++;
    }
}

int app_init(uint16_t local_addr, const char *name)
{
    (void)local_addr;
    copy_name(g_name, sizeof g_name, name);
    g_seen = calloc(BENCH_MAX_TRACKED / 8, 1);
    if (!g_seen)
        return -1;
    if (tp_bind(CHAT_PORT, app_chat_on_data, on_error) < 0)
        return -1;
    return tp_bind(BENCH_PORT, app_bench_on_data, on_error);
}

int app_bench_start(uint16_t peer, uint32_t count, size_t size, int interval_ms, int reliable)
{
    if (size < BENCH_MIN_SIZE || size > MAX_SEGMENT_PAYLOAD || count == 0 || count > BENCH_MAX_MESSAGES)
        return -1;
    memset(&g_bench, 0, sizeof g_bench);
    g_bench.active = 1;
    g_bench.peer = peer;
    g_bench.count = count;
    g_bench.size = size;
    g_bench.interval_us = (uint64_t)interval_ms * 1000ull;
    g_bench.reliable = reliable;
    return 0;
}

int app_bench_done(void) { return g_bench.active && g_bench.done; }
int app_bench_failed(void) { return g_bench.failed; }

void app_tick(uint64_t now)
{
    Bench *b = &g_bench;
    if (!b->active || b->done)
        return;
    unsigned burst = 0;
    while (b->sent < b->count && now >= b->next_us && burst < 256) {
        if (b->reliable && tp_pending() >= BENCH_MAX_PENDING)
            break;
        uint8_t msg[MAX_SEGMENT_PAYLOAD];
        msg[0] = BENCH_DATA;
        put32(msg + 1, b->sent);
        for (size_t i = 0; i < b->size - BENCH_HEADER_SIZE; i++)
            msg[BENCH_HEADER_SIZE + i] = bench_pattern(b->sent, i);
        trace("APP", "SEND", "bench id=%u tamanho=%zu B (tipo + id + padrao de teste) -> no %u porta %d",
              b->sent, b->size, b->peer, BENCH_PORT);
        int rc = tp_send(BENCH_PORT, b->peer, BENCH_PORT, msg, b->size, b->reliable);
        if (rc == -1)
            break;
        if (rc < 0) {
            g_stats.bench_send_errors++;
            b->failed = 1;
            b->done = 1;
            return;
        }
        if (b->sent == 0)
            b->start_us = now_us();
        b->sent++;
        g_stats.bench_sent++;
        b->next_us = now + b->interval_us;
        burst++;
        if (b->interval_us > 0)
            break;
    }
    if (b->sent == b->count && (!b->reliable || tp_pending() == 0)) {
        b->done = 1;
        g_stats.bench_elapsed_us = now_us() - b->start_us;
    }
}

uint64_t app_next_deadline(void)
{
    const Bench *b = &g_bench;
    if (b->active && !b->done && b->sent < b->count && b->interval_us > 0)
        return b->next_us;
    return 0;
}

void app_print_stats(FILE *out)
{
    const AppStats *s = &g_stats;
    fprintf(out, "STAT app.chat_tx=%llu\n", (unsigned long long)s->chat_tx);
    fprintf(out, "STAT app.chat_rx=%llu\n", (unsigned long long)s->chat_rx);
    fprintf(out, "STAT app.chat_rejected=%llu\n", (unsigned long long)s->chat_rejected);
    fprintf(out, "STAT app.chat_sessions=%llu\n", (unsigned long long)s->chat_sessions);
    fprintf(out, "STAT app.delivery_errors=%llu\n", (unsigned long long)s->delivery_errors);
    fprintf(out, "STAT app.bench_sent=%llu\n", (unsigned long long)s->bench_sent);
    fprintf(out, "STAT app.bench_send_errors=%llu\n", (unsigned long long)s->bench_send_errors);
    fprintf(out, "STAT app.bench_elapsed_us=%llu\n", (unsigned long long)s->bench_elapsed_us);
    fprintf(out, "STAT app.bench_rx=%llu\n", (unsigned long long)s->bench_rx);
    fprintf(out, "STAT app.bench_bytes=%llu\n", (unsigned long long)s->bench_bytes);
    fprintf(out, "STAT app.bench_unique=%llu\n", (unsigned long long)s->bench_unique);
    fprintf(out, "STAT app.bench_dups=%llu\n", (unsigned long long)s->bench_dups);
    fprintf(out, "STAT app.bench_in_order=%llu\n", (unsigned long long)s->bench_in_order);
    fprintf(out, "STAT app.bench_skipped=%llu\n", (unsigned long long)s->bench_skipped);
    fprintf(out, "STAT app.bench_reordered=%llu\n", (unsigned long long)s->bench_reordered);
    fprintf(out, "STAT app.bench_bad_payload=%llu\n", (unsigned long long)s->bench_bad_payload);
    fprintf(out, "STAT app.bench_bad_format=%llu\n", (unsigned long long)s->bench_bad_format);
}
