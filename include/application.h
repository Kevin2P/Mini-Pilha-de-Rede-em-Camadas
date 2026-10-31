#ifndef APPLICATION_H
#define APPLICATION_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CHAT_PORT 1
#define BENCH_PORT 2
#define BENCH_HEADER_SIZE 5
#define BENCH_MIN_SIZE BENCH_HEADER_SIZE
#define CHAT_MAX_TEXT 200
#define BENCH_MAX_MESSAGES (1u << 20)

enum { CHAT_HELLO = 1, CHAT_HELLO_ACK = 2, CHAT_TEXT = 3, CHAT_BYE = 4 };
enum { BENCH_DATA = 10 };

/* Binds the chat (port 1) and benchmark (port 2) applications on the transport layer. */
int app_init(uint16_t local_addr, const char *name);

/* Chat: opens a session with peer (HELLO / HELLO_ACK handshake). */
void app_chat_connect(uint16_t peer);
/* Chat: sends text once the session is established. */
void app_chat_queue_text(const char *text);
/* Chat: handles one line typed by the user; returns 1 when the user asked to quit. */
int app_chat_line(const char *line);
/* Chat: returns 1 unless queued text is still waiting for a session whose handshake has not finished. */
int app_chat_finished(void);

/* Benchmark sender: count messages (at most BENCH_MAX_MESSAGES) of size bytes, one every interval_ms (0 = as fast as the window allows). */
int app_bench_start(uint16_t peer, uint32_t count, size_t size, int interval_ms, int reliable);
int app_bench_done(void);
int app_bench_failed(void);
void app_tick(uint64_t now);
uint64_t app_next_deadline(void);

void app_chat_on_data(uint16_t src, uint16_t src_port, const uint8_t *data, size_t len);
void app_bench_on_data(uint16_t src, uint16_t src_port, const uint8_t *data, size_t len);
void app_print_stats(FILE *out);

#endif
