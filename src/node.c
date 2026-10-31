#include "application.h"
#include "common.h"
#include "link.h"
#include "network.h"
#include "transport.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

typedef enum { MODE_CHAT, MODE_SINK, MODE_SEND } Mode;

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "uso: %s -a ENDERECO -p PORTA_UDP [opcoes]\n"
        "  -a N        endereco logico deste no (1 a 65535)\n"
        "  -p N        porta UDP local (o 'meio fisico')\n"
        "  -n A=IP:P   vizinho direto: endereco logico A em IP:P (repetivel)\n"
        "  -r D=A      rota estatica: pacotes para D saem pelo vizinho A, declarado com -n (repetivel)\n"
        "  -m MODO     chat (padrao), sink (recebe o benchmark) ou send (envia o benchmark)\n"
        "  -d A        no de destino (chat: abre sessao; send: destino do benchmark)\n"
        "  -U NOME     nome do usuario no chat\n"
        "  -c TEXTO    texto de chat enviado assim que a sessao abrir\n"
        "  -N N        mensagens do benchmark (padrao 100, ate 1048576)\n"
        "  -s N        tamanho de cada mensagem em bytes, minimo 5 (padrao 100)\n"
        "  -i MS       intervalo entre mensagens (padrao 0: o mais rapido que a janela permite)\n"
        "  -u          envia o benchmark pelo transporte nao confiavel\n"
        "  -l P        probabilidade de perda de frames, de 0 a 1\n"
        "  -e P        probabilidade de corrupcao de frames, de 0 a 1\n"
        "  -D MS       atraso do enlace, em ms\n"
        "  -j MS       jitter do enlace (soma um valor uniforme de 0 a MS)\n"
        "  -x          desliga a verificacao do CRC-16 no recebimento\n"
        "  -S N        semente do gerador pseudoaleatorio (inteiro sem sinal)\n"
        "  -w N        janela do transporte, de 1 a 1024 (padrao 4)\n"
        "  -t MS       temporizador de retransmissao (padrao 200)\n"
        "  -R N        retransmissoes antes de desistir (padrao 10)\n"
        "  -T N        TTL dos pacotes (padrao 16)\n"
        "  -W S        tempo maximo do modo send, em segundos (padrao 120)\n"
        "  -E MS       modo sink: encerra apos MS sem receber nada (padrao: nunca)\n"
        "  -P          imprime estatisticas ao sair (padrao nos modos send e sink)\n"
        "  -v          mostra o encapsulamento e o desencapsulamento em cada camada\n"
        "codigos de saida: 0 sucesso, 1 erro de uso, 2 tempo maximo esgotado, 3 falha de entrega\n",
        prog);
}

static unsigned long parse_num(const char *s, char opt, unsigned long min, unsigned long max)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (*s == '\0' || *s == '-' || *end != '\0' || errno != 0 || v < min || v > max) {
        fprintf(stderr, "erro: valor invalido para -%c: '%s' (esperado de %lu a %lu)\n", opt, s, min, max);
        exit(1);
    }
    return v;
}

static uint64_t parse_seed(const char *s)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (*s < '0' || *s > '9' || *end != '\0' || errno != 0) {
        fprintf(stderr, "erro: valor invalido para -S: '%s' (esperado um inteiro sem sinal)\n", s);
        exit(1);
    }
    return v;
}

static double parse_prob(const char *s, char opt)
{
    char *end;
    double v = strtod(s, &end);
    if (*s == '\0' || *end != '\0' || !(v >= 0.0 && v <= 1.0)) {
        fprintf(stderr, "erro: valor invalido para -%c: '%s' (esperado de 0 a 1)\n", opt, s);
        exit(1);
    }
    return v;
}

static int parse_pair(const char *arg, char opt, unsigned *left, char *rest, size_t rest_len)
{
    const char *eq = strchr(arg, '=');
    if (!eq || eq == arg)
        return -1;
    char head[16];
    size_t n = (size_t)(eq - arg);
    if (n >= sizeof head)
        return -1;
    memcpy(head, arg, n);
    head[n] = '\0';
    *left = (unsigned)parse_num(head, opt, 1, 65535);
    snprintf(rest, rest_len, "%s", eq + 1);
    return 0;
}

static uint64_t min_deadline(uint64_t a, uint64_t b)
{
    if (a == 0)
        return b;
    if (b == 0)
        return a;
    return a < b ? a : b;
}

int main(int argc, char **argv)
{
    LinkConfig lcfg = { 0, 0, 0, 0, 1, 1 };
    TpConfig tcfg = { 4, 200, 10 };
    Mode mode = MODE_CHAT;
    unsigned local = 0, udp_port = 0, ttl = 16, peer = 0;
    int have_peer = 0, reliable = 1, print_stats = -1;
    unsigned count = 100, size = 100, interval_ms = 0, wall_s = 120, idle_ms = 0;
    const char *name = "user", *chat_text = NULL;
    struct { unsigned addr; char spec[64]; } neighbors[16], routes[32];
    int nn = 0, nr = 0;

    int opt;
    while ((opt = getopt(argc, argv, "a:p:n:r:m:d:U:c:N:s:i:ul:e:D:j:xS:w:t:R:T:W:E:Pvh")) != -1) {
        switch (opt) {
        case 'a': local = (unsigned)parse_num(optarg, 'a', 1, 65535); break;
        case 'p': udp_port = (unsigned)parse_num(optarg, 'p', 1, 65535); break;
        case 'n':
            if (nn >= 16 || parse_pair(optarg, 'n', &neighbors[nn].addr, neighbors[nn].spec, sizeof neighbors[nn].spec) < 0) {
                usage(argv[0]);
                return 1;
            }
            nn++;
            break;
        case 'r':
            if (nr >= 32 || parse_pair(optarg, 'r', &routes[nr].addr, routes[nr].spec, sizeof routes[nr].spec) < 0) {
                usage(argv[0]);
                return 1;
            }
            nr++;
            break;
        case 'm':
            if (strcmp(optarg, "chat") == 0) mode = MODE_CHAT;
            else if (strcmp(optarg, "sink") == 0) mode = MODE_SINK;
            else if (strcmp(optarg, "send") == 0) mode = MODE_SEND;
            else { usage(argv[0]); return 1; }
            break;
        case 'd': peer = (unsigned)parse_num(optarg, 'd', 1, 65535); have_peer = 1; break;
        case 'U': name = optarg; break;
        case 'c': chat_text = optarg; break;
        case 'N': count = (unsigned)parse_num(optarg, 'N', 1, BENCH_MAX_MESSAGES); break;
        case 's': size = (unsigned)parse_num(optarg, 's', 5, MAX_SEGMENT_PAYLOAD); break;
        case 'i': interval_ms = (unsigned)parse_num(optarg, 'i', 0, 3600000); break;
        case 'u': reliable = 0; break;
        case 'l': lcfg.loss = parse_prob(optarg, 'l'); break;
        case 'e': lcfg.corrupt = parse_prob(optarg, 'e'); break;
        case 'D': lcfg.delay_ms = (int)parse_num(optarg, 'D', 0, 60000); break;
        case 'j': lcfg.jitter_ms = (int)parse_num(optarg, 'j', 0, 60000); break;
        case 'x': lcfg.verify_checksum = 0; break;
        case 'S': lcfg.seed = parse_seed(optarg); break;
        case 'w': tcfg.window = (int)parse_num(optarg, 'w', 1, 1024); break;
        case 't': tcfg.rto_ms = (int)parse_num(optarg, 't', 1, 600000); break;
        case 'R': tcfg.max_retries = (int)parse_num(optarg, 'R', 0, 1000000); break;
        case 'T': ttl = (unsigned)parse_num(optarg, 'T', 1, 255); break;
        case 'W': wall_s = (unsigned)parse_num(optarg, 'W', 1, 86400); break;
        case 'E': idle_ms = (unsigned)parse_num(optarg, 'E', 0, 86400000); break;
        case 'P': print_stats = 1; break;
        case 'v': g_trace = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (local == 0 || udp_port == 0 || ttl == 0 || ttl > 255) {
        usage(argv[0]);
        return 1;
    }
    if (print_stats < 0)
        print_stats = mode != MODE_CHAT;

    if (link_init(&lcfg, (uint16_t)udp_port) < 0) {
        fprintf(stderr, "erro: nao foi possivel abrir a porta UDP %u\n", udp_port);
        return 1;
    }
    net_init((uint16_t)local, (uint8_t)ttl);
    tp_init(&tcfg);
    link_set_upper(net_receive);
    net_set_upper(tp_input);
    if (app_init((uint16_t)local, name) < 0)
        return 1;

    for (int i = 0; i < nn; i++) {
        char ip[48];
        char *colon = strrchr(neighbors[i].spec, ':');
        if (!colon || colon == neighbors[i].spec || (size_t)(colon - neighbors[i].spec) >= sizeof ip) {
            fprintf(stderr, "erro: vizinho invalido '%s' (esperado ENDERECO=IP:PORTA)\n", neighbors[i].spec);
            return 1;
        }
        memcpy(ip, neighbors[i].spec, (size_t)(colon - neighbors[i].spec));
        ip[colon - neighbors[i].spec] = '\0';
        unsigned port = (unsigned)parse_num(colon + 1, 'n', 1, 65535);
        if (link_add_neighbor((uint16_t)neighbors[i].addr, ip, (uint16_t)port) < 0) {
            fprintf(stderr, "erro: vizinho invalido '%s' (esperado ENDERECO=IP:PORTA)\n", neighbors[i].spec);
            return 1;
        }
        if (net_add_route((uint16_t)neighbors[i].addr, (uint16_t)neighbors[i].addr) < 0) {
            fprintf(stderr, "erro: tabela de rotas cheia\n");
            return 1;
        }
    }
    for (int i = 0; i < nr; i++) {
        unsigned hop = (unsigned)parse_num(routes[i].spec, 'r', 1, 65535);
        int is_neighbor = 0;
        for (int k = 0; k < nn; k++)
            if (neighbors[k].addr == hop)
                is_neighbor = 1;
        if (!is_neighbor) {
            fprintf(stderr, "erro: rota invalida '%u=%s': o proximo salto %u nao e vizinho direto\n", routes[i].addr, routes[i].spec, hop);
            return 1;
        }
        if (net_add_route((uint16_t)routes[i].addr, (uint16_t)hop) < 0) {
            fprintf(stderr, "erro: tabela de rotas cheia\n");
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (mode == MODE_SEND) {
        if (!have_peer || app_bench_start((uint16_t)peer, count, size, (int)interval_ms, reliable) < 0) {
            fprintf(stderr, "erro: o modo send precisa de -d e de um tamanho entre 5 e %d\n", MAX_SEGMENT_PAYLOAD);
            return 1;
        }
    }
    if (chat_text)
        app_chat_queue_text(chat_text);
    if ((mode == MODE_CHAT || (mode == MODE_SEND && chat_text)) && have_peer)
        app_chat_connect((uint16_t)peer);

    int stdin_open = mode == MODE_CHAT;
    int quitting = 0, exit_code = 0;
    char line[512];
    size_t line_len = 0;
    uint64_t t_start = now_us(), finish_us = 0, last_rx_us = now_us();
    uint64_t last_frames_rx = 0;

    while (!g_stop) {
        uint64_t now = now_us();
        uint64_t dl = min_deadline(min_deadline(tp_next_deadline(), link_next_deadline()), app_next_deadline());
        long timeout_us = 50000;
        if (dl != 0)
            timeout_us = dl <= now ? 0 : ((long)(dl - now) < timeout_us ? (long)(dl - now) : timeout_us);

        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(link_fd(), &rs);
        int maxfd = link_fd();
        if (stdin_open)
            FD_SET(0, &rs);
        struct timeval tv = { timeout_us / 1000000, timeout_us % 1000000 };
        int r = select(maxfd + 1, &rs, NULL, NULL, &tv);
        if (r < 0 && errno != EINTR)
            break;

        if (r > 0 && FD_ISSET(link_fd(), &rs))
            link_on_readable();
        if (r > 0 && stdin_open && FD_ISSET(0, &rs)) {
            char buf[256];
            ssize_t n = read(0, buf, sizeof buf);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                n = 0;
            } else if (n <= 0) {
                stdin_open = 0;
                app_chat_line("/quit");
                quitting = 1;
            }
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    if (line_len > 0 && line[line_len - 1] == '\r')
                        line_len--;
                    line[line_len] = '\0';
                    if (line_len > 0 && app_chat_line(line))
                        quitting = 1;
                    line_len = 0;
                } else if (line_len < sizeof line - 1) {
                    line[line_len++] = buf[i];
                }
            }
        }

        now = now_us();
        link_tick(now);
        tp_tick(now);
        app_tick(now);

        if (link_stats()->frames_rx != last_frames_rx) {
            last_frames_rx = link_stats()->frames_rx;
            last_rx_us = now;
        }
        if (mode == MODE_SEND) {
            if (now - t_start > (uint64_t)wall_s * 1000000ull) {
                exit_code = 2;
                break;
            }
            if (app_bench_done() && app_chat_finished() && tp_pending() == 0) {
                if (finish_us == 0)
                    finish_us = now;
                uint64_t grace = reliable ? 0 : 300000;
                if (link_queued() == 0 && now - finish_us >= grace)
                    break;
            }
        } else if (mode == MODE_SINK) {
            if (idle_ms > 0 && now - last_rx_us > (uint64_t)idle_ms * 1000ull)
                break;
        } else if (quitting && app_chat_finished() && tp_pending() == 0 && link_queued() == 0) {
            break;
        }
    }

    if (exit_code == 0 && app_bench_failed())
        exit_code = 3;

    if (print_stats) {
        link_print_stats(stdout);
        net_print_stats(stdout);
        tp_print_stats(stdout);
        app_print_stats(stdout);
        printf("STAT node.bench_failed=%d\n", app_bench_failed());
        printf("STAT node.exit_code=%d\n", exit_code);
    }
    return exit_code;
}
