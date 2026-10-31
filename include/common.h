#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>

#define MAX_FRAME 1500
#define MAX_SEGMENT_PAYLOAD 1000

extern int g_trace;

uint64_t now_us(void);
void put16(uint8_t *p, uint16_t v);
uint16_t get16(const uint8_t *p);
void put32(uint8_t *p, uint32_t v);
uint32_t get32(const uint8_t *p);
void trace(const char *layer, const char *dir, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif
