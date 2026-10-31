#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define TP_HEADER_SIZE 13
#define TP_FLAG_DATA 0x01
#define TP_FLAG_ACK 0x02
#define TP_FLAG_REL 0x04

#define TP_ERR_TIMEOUT 1

typedef struct {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t flags;
} TpHeader;

typedef struct {
    int window;
    int rto_ms;
    int max_retries;
} TpConfig;

typedef struct {
    uint64_t segments_tx, retransmissions, timeouts, unrel_tx, acks_tx, acks_rx, dup_acks_rx, bad_acks;
    uint64_t delivered, dup_rx, out_of_order_dropped, unrel_gaps, unrel_reordered;
    uint64_t rtt_samples, rtt_sum_us, rx_bad_header, rx_no_port, rx_flow_table_full, failed_flows;
} TpStats;

/* Called with the payload of every segment delivered to a bound port. */
typedef void (*tp_data_fn)(uint16_t src_addr, uint16_t src_port, const uint8_t *data, size_t len);
/* Called when a reliable flow gives up after max_retries retransmissions. */
typedef void (*tp_error_fn)(uint16_t dst_addr, uint16_t dst_port, int err);

int tp_init(const TpConfig *cfg);
/* Reserves a logical port for an application (multiplexing key). */
int tp_bind(uint16_t port, tp_data_fn on_data, tp_error_fn on_error);
/* Sends one message. Returns 0, -1 if the send queue is full, -2 on invalid arguments, -3 if the flow failed. */
int tp_send(uint16_t local_port, uint16_t dst_addr, uint16_t dst_port,
            const uint8_t *data, size_t len, int reliable);
/* Reliable segments queued or still unacknowledged, over all flows. */
size_t tp_pending(void);
/* Entry point for segments coming from the network layer. */
void tp_input(uint16_t src_addr, const uint8_t *segment, size_t len);
void tp_tick(uint64_t now);
uint64_t tp_next_deadline(void);
const TpStats *tp_stats(void);
void tp_print_stats(FILE *out);

void tp_encode_header(uint8_t *out, const TpHeader *h);
int tp_decode_header(const uint8_t *in, size_t len, TpHeader *h);

#endif
