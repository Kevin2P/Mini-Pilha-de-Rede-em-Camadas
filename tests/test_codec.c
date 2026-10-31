#include "application.h"
#include "common.h"
#include "link.h"
#include "network.h"
#include "transport.h"

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass, g_fail;

static void check(int cond, const char *msg)
{
    if (cond) {
        g_pass++;
        printf("  ok   %s\n", msg);
    } else {
        g_fail++;
        printf("  FALHOU %s\n", msg);
    }
}

static void test_crc(void)
{
    const uint8_t v[] = "123456789";
    check(crc16(v, 9) == 0x29B1, "CRC-16/CCITT-FALSE do vetor padrao '123456789' = 0x29B1");
}

static void test_link(void)
{
    uint8_t pkt[100], frame[MAX_FRAME];
    for (int i = 0; i < 100; i++)
        pkt[i] = (uint8_t)(i * 7);
    size_t flen = link_encode_frame(frame, pkt, sizeof pkt);
    const uint8_t *out;
    size_t olen;
    check(flen == 100 + LINK_HEADER_SIZE, "frame tem cabecalho de 4 bytes + pacote");
    check(link_decode_frame(frame, flen, 1, &out, &olen) == 0 && olen == 100 && memcmp(out, pkt, 100) == 0,
          "encapsular e desencapsular o frame devolve o mesmo pacote");

    int detected = 0, total = 0;
    for (size_t bit = 0; bit < flen * 8; bit++) {
        uint8_t copy[MAX_FRAME];
        memcpy(copy, frame, flen);
        copy[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        total++;
        if (link_decode_frame(copy, flen, 1, &out, &olen) != 0)
            detected++;
    }
    check(detected == total, "qualquer inversao de 1 bit no frame e detectada (CRC ou tamanho)");

    check(link_decode_frame(frame, flen - 1, 1, &out, &olen) == LINK_ERR_LENGTH, "frame truncado e rejeitado");
    check(link_decode_frame(frame, 2, 1, &out, &olen) == LINK_ERR_LENGTH, "frame menor que o cabecalho e rejeitado");
}

static void test_network(void)
{
    NetHeader h = { NET_VERSION, 9, NET_PROTO_TRANSPORT, 1234, 4321, 55 }, d;
    uint8_t buf[NET_HEADER_SIZE + 55];
    memset(buf, 0, sizeof buf);
    net_encode_header(buf, &h);
    check(net_decode_header(buf, sizeof buf, &d) == 0 && d.src == 1234 && d.dst == 4321 && d.ttl == 9 &&
              d.payload_len == 55 && d.protocol == NET_PROTO_TRANSPORT,
          "cabecalho de rede: ida e volta preserva todos os campos");
    check(net_decode_header(buf, sizeof buf - 1, &d) < 0, "cabecalho de rede com tamanho inconsistente e rejeitado");
    buf[0] = 7;
    check(net_decode_header(buf, sizeof buf, &d) < 0, "versao desconhecida e rejeitada");
}

static void test_transport(void)
{
    TpHeader h = { 11, 22, 0xDEADBEEF, 0x01020304, TP_FLAG_DATA | TP_FLAG_REL }, d;
    uint8_t buf[TP_HEADER_SIZE + 10];
    memset(buf, 0, sizeof buf);
    tp_encode_header(buf, &h);
    check(tp_decode_header(buf, sizeof buf, &d) == 0 && d.src_port == 11 && d.dst_port == 22 &&
              d.seq == 0xDEADBEEF && d.ack == 0x01020304 && d.flags == (TP_FLAG_DATA | TP_FLAG_REL),
          "cabecalho de transporte: ida e volta preserva todos os campos");
    check(tp_decode_header(buf, TP_HEADER_SIZE - 1, &d) < 0, "segmento menor que o cabecalho e rejeitado");
    buf[12] = 0;
    check(tp_decode_header(buf, sizeof buf, &d) < 0, "segmento sem flag DATA nem ACK e rejeitado");
}

static void fuzz(void)
{
    LinkConfig lcfg = { 0, 0, 0, 0, 1, 5 };
    TpConfig tcfg = { 4, 200, 10 };
    link_init(&lcfg, 0);
    net_init(7, 16);
    tp_init(&tcfg);
    net_set_upper(tp_input);
    app_init(7, "fuzz");
    net_add_route(9, 9);

    fflush(stdout);
    int saved = dup(1);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 1);
    close(devnull);
    srand(12345);
    uint8_t buf[MAX_FRAME];
    const uint8_t *pkt;
    size_t plen;
    int rounds = 200000;
    for (int i = 0; i < rounds; i++) {
        size_t n = (size_t)(rand() % 200);
        for (size_t k = 0; k < n; k++)
            buf[k] = (uint8_t)rand();
        if (n >= NET_HEADER_SIZE && i % 2 == 0) {
            buf[0] = NET_VERSION;
            put16(buf + 4, 7);
            buf[6] = NET_PROTO_TRANSPORT;
            put16(buf + 7, (uint16_t)(n - NET_HEADER_SIZE));
            if (n > NET_HEADER_SIZE + TP_HEADER_SIZE && i % 4 == 0)
                buf[NET_HEADER_SIZE + 12] = TP_FLAG_DATA | (uint8_t)(rand() & TP_FLAG_REL);
        }
        link_decode_frame(buf, n, 1, &pkt, &plen);
        net_receive(9, buf, n);
        app_chat_on_data(9, 1, buf, n % 300);
        app_bench_on_data(9, 2, buf, n);
    }
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    check(1, "200000 entradas aleatorias nas quatro camadas sem falha de memoria");
}

int main(void)
{
    printf("=== codec e robustez ===\n");
    test_crc();
    test_link();
    test_network();
    test_transport();
    fuzz();
    printf("\nresultado: %d passaram, %d falharam\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("TODOS OS TESTES PASSARAM\n");
    return g_fail != 0;
}
