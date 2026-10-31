#ifndef LINK_H
#define LINK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LINK_HEADER_SIZE 4
#define LINK_ERR_LENGTH (-1)
#define LINK_ERR_CHECKSUM (-2)

typedef struct {
    double loss;
    double corrupt;
    int delay_ms;
    int jitter_ms;
    int verify_checksum;
    uint64_t seed;
} LinkConfig;

typedef struct {
    uint64_t frames_tx, bytes_tx, frames_lost_sim, frames_corrupted_sim;
    uint64_t frames_delayed, queue_overflow, tx_errors;
    uint64_t frames_rx, bytes_rx, rx_bad_checksum, rx_bad_length, rx_unknown_neighbor;
} LinkStats;

typedef void (*link_rx_fn)(uint16_t from, const uint8_t *packet, size_t len);

/* Opens the UDP socket used as the physical medium; udp_port 0 picks a free port. */
int link_init(const LinkConfig *cfg, uint16_t udp_port);
/* Registers a directly connected node reachable at ip:udp_port. */
int link_add_neighbor(uint16_t addr, const char *ip, uint16_t udp_port);
int link_fd(void);
/* Upper layer callback: receives the payload of every valid frame. */
void link_set_upper(link_rx_fn fn);
/* Frames the packet (length and CRC-16), applies the simulated loss, corruption and delay, sends to the neighbor. */
int link_send(uint16_t neighbor, const uint8_t *packet, size_t len);
/* Reads up to 512 pending datagrams; call when link_fd() is readable (the socket stays readable if more remain). */
void link_on_readable(void);
/* Releases frames whose simulated delay has elapsed. */
void link_tick(uint64_t now);
uint64_t link_next_deadline(void);
size_t link_queued(void);
const LinkStats *link_stats(void);
void link_print_stats(FILE *out);

uint16_t crc16(const uint8_t *data, size_t len);
size_t link_encode_frame(uint8_t *out, const uint8_t *packet, size_t len);
/* Returns 0, LINK_ERR_LENGTH (inconsistent length or frame larger than MAX_FRAME) or LINK_ERR_CHECKSUM; on success the payload is returned through packet and len. */
int link_decode_frame(const uint8_t *frame, size_t frame_len, int verify,
                      const uint8_t **packet, size_t *len);

#endif
