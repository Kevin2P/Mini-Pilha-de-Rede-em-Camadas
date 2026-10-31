import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import conformance  # noqa: E402
import math  # noqa: E402
import re  # noqa: E402
import socket  # noqa: E402
import subprocess  # noqa: E402
import time  # noqa: E402
from harness import BIN, forge_frame, next_ports, parse_stats, run_once, run_transfer, start, stop  # noqa: E402

passed = 0
failed = 0


def check(cond, msg):
    global passed, failed
    if cond:
        passed += 1
        print("  ok   " + msg)
    else:
        failed += 1
        print("  FALHOU " + msg)


def section(title):
    print("\n=== %s ===" % title)


if not os.path.exists(BIN):
    print("binario %s nao encontrado; rode 'make' antes" % BIN)
    sys.exit(2)

N = 60

section("separacao de camadas (cada modulo so inclui a camada logo abaixo)")
ALLOWED = {
    "application.c": {"application.h", "common.h", "transport.h"},
    "transport.c": {"transport.h", "common.h", "network.h"},
    "network.c": {"network.h", "common.h", "link.h"},
    "link.c": {"link.h", "common.h"},
}
import re  # noqa: E402
src_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")
for fname, allowed in ALLOWED.items():
    with open(os.path.join(src_dir, fname)) as fh:
        includes = set(re.findall(r'#include "([^"]+)"', fh.read()))
    check(includes <= allowed, "%s depende apenas de %s" % (fname, ", ".join(sorted(allowed - {"common.h"}))))

section("transporte confiavel sem perdas")
r = run_transfer(count=N, seed=1)
check(r["tx_rc"] == 0, "emissor termina com sucesso")
check(r["rx"]["app.bench_unique"] == N and r["rx"]["app.bench_in_order"] == N, "todas as %d mensagens chegam, em ordem" % N)
check(r["tx"]["tp.retransmissions"] == 0, "nenhuma retransmissao quando nao ha perdas")
check(r["rx"]["app.bench_dups"] == 0 and r["rx"]["app.bench_bad_payload"] == 0, "sem duplicatas nem dados corrompidos na aplicacao")

section("transporte confiavel com 25% de perda de frames")
r = run_transfer(count=N, loss=0.25, delay=2, seed=2)
check(r["tx_rc"] == 0 and r["tx"]["node.bench_failed"] == 0, "emissor conclui a transferencia")
check(r["rx"]["app.bench_unique"] == N and r["rx"]["app.bench_in_order"] == N, "ACK e retransmissao entregam tudo, em ordem")
check(r["rx"]["app.bench_dups"] == 0, "duplicatas sao filtradas pelo transporte (aplicacao nao ve repeticao)")
check(r["tx"]["tp.retransmissions"] > 0 and r["tx"]["link.frames_lost_sim"] + r["rx"]["link.frames_lost_sim"] > 0,
      "houve perdas simuladas e retransmissoes")

section("transporte nao confiavel com 30% de perda")
r = run_transfer(count=N * 2, reliable=False, loss=0.30, seed=3)
check(r["tx"]["tp.retransmissions"] == 0 and r["tx"]["tp.unrel_tx"] == N * 2, "sem ACK e sem retransmissao")
check(0 < r["rx"]["app.bench_unique"] < N * 2, "parte das mensagens se perde (%d de %d chegaram)" % (r["rx"]["app.bench_unique"], N * 2))
check(r["rx"]["tp.unrel_gaps"] > 0, "o receptor detecta lacunas na numeracao")

section("deteccao de erros no enlace (corrupcao de 20% dos frames)")
r = run_transfer(count=N, corrupt=0.20, delay=1, seed=4)
check(r["rx"]["link.rx_bad_checksum"] + r["tx"]["link.rx_bad_checksum"] > 0, "CRC-16 detecta frames corrompidos")
check(r["rx"]["app.bench_bad_payload"] == 0 and r["rx"]["app.bench_unique"] == N, "com CRC, nenhum dado corrompido chega a aplicacao e tudo e entregue")
r = run_transfer(count=N, corrupt=0.30, delay=1, seed=5, verify=False, rto=60, retries=10, wall=30)
check(r["rx"]["app.bench_bad_payload"] + r["rx"]["app.bench_bad_format"] > 0, "sem CRC, dados corrompidos chegam a aplicacao")

section("encaminhamento em 3 nos (1 -> 2 -> 3) e TTL")
r = run_transfer(count=N, relay=True, delay=1, seed=6)
check(r["tx_rc"] == 0 and r["rx"]["app.bench_unique"] == N, "no 3 recebe tudo atraves do no intermediario")
check(r["relay"]["net.forwarded"] >= 2 * N, "o no 2 encaminhou dados e ACKs (%d pacotes)" % r["relay"]["net.forwarded"])
r = run_transfer(count=10, reliable=False, relay=True, ttl=1, seed=7)
check(r["rx"].get("app.bench_unique", 0) == 0 and r["relay"]["net.ttl_expired"] >= 10, "com TTL 1 o no intermediario descarta os pacotes")

section("multiplexacao: chat (porta 1) e benchmark (porta 2) no mesmo no")
r = run_transfer(count=N, delay=1, seed=8, chat_text="mensagem de teste")
check(r["rx"]["app.bench_unique"] == N, "benchmark entregue na porta 2")
check("[user@1] mensagem de teste" in r["rx_out"] and "sessao iniciada por 1" in r["rx_out"], "chat entregue na porta 1, com sessao estabelecida")

section("validacao de argumentos e codigos de saida")
b = next_ports()
base = ["-a", "1", "-p", str(b)]
check(run_once(["-a", "70000", "-p", str(b)]).returncode == 1, "endereco acima de 65535 e rejeitado")
check(run_once(["-a", "1", "-p", "0"]).returncode == 1, "porta UDP 0 e rejeitada")
check(run_once(base + ["-l", "5"]).returncode == 1, "probabilidade de perda fora de 0 a 1 e rejeitada")
check(run_once(base + ["-t", "0"]).returncode == 1, "temporizador de 0 ms e rejeitado")
check(run_once(base + ["-n", "2=127.0.0.1:99999"]).returncode == 1, "porta de vizinho acima de 65535 e rejeitada")
check(run_once(base + ["-m", "send", "-d", "2", "-s", "1001"]).returncode == 1, "mensagem maior que o segmento e rejeitada")
r = run_once(base + ["-n", "2=127.0.0.1:%d" % (b + 1), "-m", "send", "-d", "2", "-N", "5", "-t", "30", "-R", "3"])
st = parse_stats(r.stdout)
check(r.returncode == 3 and st["node.bench_failed"] == 1 and st["app.delivery_errors"] == 1,
      "destino inexistente: o transporte desiste, avisa a aplicacao e o processo sai com codigo 3")
r = run_once(["-a", "1", "-p", str(next_ports()), "-n", "2=127.0.0.1:%d" % next_ports(), "-m", "send", "-d", "2", "-N", "5", "-t", "1000",
              "-R", "100", "-W", "1"])
check(r.returncode == 2, "tempo maximo do modo send esgotado: codigo de saida 2")

section("ritmo de envio e chat fora de sessao")
r = run_transfer(count=20, interval=20, seed=9)
check(r["tx"]["app.bench_elapsed_us"] >= 380000 and r["rx"]["app.bench_unique"] == 20, "intervalo de 20 ms entre mensagens e respeitado")
b = next_ports()
node = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-U", "bob", "-P"], stdin=True)
time.sleep(0.3)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", b))
sock.sendto(forge_frame(1, 2, 1, 1, 0, bytes([3]) + b"texto sem sessao"), ("127.0.0.1", b + 1))
time.sleep(0.3)
sock.close()
out, _ = stop(node)
st = parse_stats(out)
check(st["app.chat_rejected"] == 1 and st["app.chat_rx"] == 0, "texto de chat sem sessao estabelecida e rejeitado e contado")


section("integridade do benchmark: identificador adulterado nos bytes altos")
def bench_msg(msg_id, pattern_id, size=40):
    import struct
    body = bytes([10]) + struct.pack(">I", msg_id)
    fold = lambda v: (v ^ (v >> 8) ^ (v >> 16) ^ (v >> 24)) & 0xFF
    return body + bytes(((fold(pattern_id) + i) & 0xFF) for i in range(size - 5))
b = next_ports()
node = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-m", "sink"])
time.sleep(0.3)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", b))
sock.sendto(forge_frame(1, 2, 2, 2, 0, bench_msg(5, 5)), ("127.0.0.1", b + 1))
sock.sendto(forge_frame(1, 2, 2, 2, 1, bench_msg(5 | (1 << 20), 5)), ("127.0.0.1", b + 1))
time.sleep(0.3)
sock.close()
out, _ = stop(node)
st = parse_stats(out)
check(st["app.bench_rx"] == 1 and st["app.bench_bad_payload"] == 1, "mensagem valida e aceita; a mesma com o bit 20 do identificador invertido e rejeitada")


section("relacoes medidas no relatorio (valem em qualquer maquina)")
for reliable, extra in ((True, 52), (False, 26)):
    exact = True
    for size in (10, 1000):
        r = run_transfer(count=20, size=size, reliable=reliable, loss=0.0, delay=0, rto=500, seed=1)
        wire = r["tx"]["link.bytes_tx"] + r["rx"]["link.bytes_tx"]
        exact &= abs(wire / (20.0 * size) - (size + extra) / float(size)) < 1e-9
    check(exact, "overhead no modo %s e exatamente (S + %d)/S" % ("confiavel" if reliable else "nao confiavel", extra))
for w in (1, 4, 16):
    r = run_transfer(count=40, size=100, delay=20, window=w, rto=140, retries=40, seed=1)
    rtt = r["tx"]["tp.rtt_sum_us"] / r["tx"]["tp.rtt_samples"] / 1000.0
    model = math.ceil(40.0 / w) * rtt
    t = r["tx"]["app.bench_elapsed_us"] / 1000.0
    check(abs(t - model) / model < 0.15, "janela %d: tempo %.0f ms, modelo ceil(N/janela) x RTT = %.0f ms (erro abaixo de 15%%)" % (w, t, model))
rates = {}
for w in (1, 16):
    r = run_transfer(count=60, size=100, loss=0.10, delay=20, window=w, rto=140, retries=80, seed=1, wall=60)
    rates[w] = 100.0 * r["tx"]["tp.retransmissions"] / r["tx"]["tp.segments_tx"]
wide = run_transfer(count=600, size=20, delay=20, window=256, rto=140, retries=40, seed=1)
wide_rtt = wide["tx"]["tp.rtt_sum_us"] / wide["tx"]["tp.rtt_samples"] / 1000.0
wide_model = math.ceil(600 / 256.0) * wide_rtt
wide_time = wide["tx"]["app.bench_elapsed_us"] / 1000.0
check(abs(wide_time - wide_model) / wide_model < 0.25, "janela 256 (acima de 64) tambem segue o modelo: %.0f ms contra %.0f ms previstos" % (wide_time, wide_model))
check(rates[16] > rates[1] + 10, "com 10%% de perda a janela 16 retransmite mais que a janela 1 (%s%% contra %s%%)" % (("%.1f" % rates[16]).replace(".", ","), ("%.1f" % rates[1]).replace(".", ",")))


section("regressoes de seguranca e robustez")
b = next_ports()
node = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-n", "3=127.0.0.1:%d" % (b + 2), "-m", "sink"])
time.sleep(0.3)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", b))
sock.sendto(forge_frame(1, 3, 1, 1, 0, b"A" * 1538), ("127.0.0.1", b + 1))
time.sleep(0.3)
sock.close()
out, _ = stop(node)
st = parse_stats(out)
check(st.get("link.rx_bad_length", -1) == 1 and st.get("net.forwarded", -1) == 0, "frame de 1564 B, maior que MAX_FRAME, e descartado no enlace sem estourar o buffer de encaminhamento")

b = next_ports()
node = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-U", "bob"], stdin=True)
time.sleep(0.3)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", b))
dest = ("127.0.0.1", b + 1)
sock.sendto(forge_frame(1, 2, 1, 1, 0, bytes([1]) + ("a" * 31 + "\u00e9").encode()), dest)
sock.sendto(forge_frame(1, 2, 1, 1, 1, bytes([3]) + b"ola \xc2\x9b31m\xc2\x9d0;titulo fim"), dest)
time.sleep(0.5)
sock.close()
node.stdin.write("/quit\n")
node.stdin.flush()
out, _ = stop(node)
check("\u009b" not in out and "\u009d" not in out and "ola" in out, "controles C1 de um peer (U+009B, U+009D) nao chegam ao terminal")
check("\ufffd" not in out, "o nome com 33 bytes e truncado numa fronteira de caractere UTF-8")

garbage = [run_once(["-a", "1", "-p", str(next_ports()), "-S", v, "-m", "sink", "-E", "50"]).returncode for v in ("abc", "-1", "12x", "")]
check(all(rc == 1 for rc in garbage), "-S rejeita texto, sinal negativo, lixo no fim e valor vazio")

b = next_ports()
bob = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-U", "bob"], stdin=True)
alice = start(["-a", "1", "-p", str(b), "-n", "2=127.0.0.1:%d" % (b + 1), "-d", "2", "-U", "alice"], stdin=True)
time.sleep(0.8)
alice.stdin.write("ola\r\n")
alice.stdin.flush()
time.sleep(0.6)
alice.stdin.write("/quit\n")
alice.stdin.flush()
alice.communicate(timeout=10)
bob.stdin.write("/quit\n")
bob.stdin.flush()
bob_out, _ = stop(bob)
check("[alice@1] ola\n" in bob_out, "o CR de uma linha com CRLF e removido")

delivered = 0
for k in range(5):
    b = next_ports()
    bob = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-U", "bob", "-j", "60", "-D", "5", "-S", str(k + 1)], stdin=True)
    time.sleep(0.3)
    run_once(["-a", "1", "-p", str(b), "-n", "2=127.0.0.1:%d" % (b + 1), "-d", "2", "-U", "alice", "-c", "texto-%d" % k, "-j", "60", "-D", "5", "-S", str(k + 1)], timeout=30)
    time.sleep(0.3)
    bob_out, _ = stop(bob)
    delivered += ("texto-%d" % k) in bob_out
check(delivered == 5, "o texto de -c e entregue mesmo com a entrada fechada e jitter de 60 ms (%d de 5)" % delivered)

b = next_ports()
r = run_once(["-a", "1", "-p", str(b), "-n", "2=127.0.0.1:%d" % (b + 1), "-d", "2", "-c", "x", "-t", "50", "-R", "2"], timeout=20)
check(r.returncode == 0, "com o destino ausente, o no em modo chat com -c e entrada fechada encerra sozinho")

b = next_ports()
r = run_once(["-a", "1", "-p", str(b), "-n", "2=127.0.0.1:%d" % (b + 1), "-r", "3=9", "-m", "send", "-d", "3"])
check(r.returncode == 1 and "nao e vizinho direto" in r.stderr, "rota cujo proximo salto nao e vizinho direto e rejeitada na partida")


section("caminhos de erro, limites e opcoes pouco usadas")
helped = run_once(["-h"])
check(helped.returncode == 0 and "uso:" in helped.stderr, "-h imprime o uso e sai com codigo 0")
check(run_once(["-a", "1", "-p", str(next_ports()), "-Z"]).returncode == 1, "opcao desconhecida sai com codigo 1")
check(run_once(["-a", "1", "-p", str(next_ports()), "-m", "outro"]).returncode == 1, "modo desconhecido sai com codigo 1")
malformed = ["2=semporta", "2=999.1.1.1:5", "=127.0.0.1:5", "abc=127.0.0.1:5", "2=127.0.0.1:0"]
check(all(run_once(["-a", "1", "-p", str(next_ports()), "-n", v]).returncode == 1 for v in malformed), "vizinhos malformados sao rejeitados na partida")
b = next_ports()
busy = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
busy.bind(("0.0.0.0", b))
taken = run_once(["-a", "1", "-p", str(b)])
busy.close()
check(taken.returncode == 1 and "porta UDP" in taken.stderr, "porta UDP ocupada: erro claro e codigo 1")
check(run_once(["-a", "1", "-p", str(next_ports()), "-n", "2=127.0.0.1:9", "-m", "send"]).returncode == 1, "modo send sem -d sai com codigo 1")
too_many = run_once(["-a", "1", "-p", str(next_ports()), "-n", "2=127.0.0.1:9", "-m", "send", "-d", "2", "-N", "1048577"])
check(too_many.returncode == 1 and "valor invalido para -N" in too_many.stderr, "-N acima de 1048576 mensagens e rejeitado, pois o receptor so rastreia ids abaixo disso")
b = next_ports()
t0 = time.time()
idle = run_once(["-a", "2", "-p", str(b), "-n", "1=127.0.0.1:%d" % (b + 1), "-n", "3=127.0.0.1:%d" % (b + 2), "-r", "4=1", "-r", "4=3", "-m", "sink", "-E", "300"], timeout=10)
check(idle.returncode == 0 and 0.25 < time.time() - t0 < 3 and "STAT node.exit_code=0" in idle.stdout, "sink com -E encerra sozinho apos o tempo sem trafego e aceita rota repetida")

b = next_ports()
flood = run_once(["-a", "1", "-p", str(b), "-n", "2=127.0.0.1:%d" % (b + 1), "-m", "send", "-d", "2", "-N", "3000", "-s", "20", "-u", "-D", "2500", "-W", "30"], timeout=40)
fst = parse_stats(flood.stdout)
check(fst.get("link.frames_delayed") == 2048 and fst.get("link.queue_overflow") == 952, "a fila de atraso guarda no maximo 2048 frames e conta os excedentes (952 de 3000)")

b = next_ports()
node = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-m", "sink"])
time.sleep(0.3)
stranger = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
stranger.bind(("127.0.0.1", b + 3))
stranger.sendto(forge_frame(1, 2, 2, 2, 0, bytes([10]) + b"\x00\x00\x00\x00" + bytes(15)), ("127.0.0.1", b + 1))
time.sleep(0.3)
stranger.close()
out, _ = stop(node)
st = parse_stats(out)
check(st.get("link.rx_unknown_neighbor") == 1 and st.get("link.frames_rx") == 0 and st.get("tp.delivered") == 0, "frame de origem que nao e vizinho configurado e ignorado")

b = next_ports()
node = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-U", "bob", "-P"], stdin=True)
time.sleep(0.3)
peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
peer.bind(("127.0.0.1", b))
peer.sendto(forge_frame(1, 2, 1, 1, 0, bytes([2]) + b"intruso"), ("127.0.0.1", b + 1))
time.sleep(0.4)
peer.close()
node.stdin.write("/quit\n")
node.stdin.flush()
out, _ = stop(node)
st = parse_stats(out)
check(st.get("app.chat_sessions") == 0 and "sessao estabelecida" not in out, "HELLO_ACK sem HELLO enviado e ignorado")


section("sessao do chat, jitter e limite de retransmissoes")
b = next_ports()
node = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-U", "bob", "-P"], stdin=True)
time.sleep(0.3)
peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
peer.bind(("127.0.0.1", b))
for seq, body in enumerate((bytes([1]) + b"carol", bytes([4]), bytes([3]) + b"depois do bye")):
    peer.sendto(forge_frame(1, 2, 1, 1, seq, body), ("127.0.0.1", b + 1))
    time.sleep(0.15)
peer.close()
node.stdin.write("/quit\n")
node.stdin.flush()
out, _ = stop(node)
st = parse_stats(out)
check("depois do bye" not in out and "sessao encerrada" in out and st.get("app.chat_rejected") == 1, "texto recebido depois do BYE e rejeitado e contado")

jit = run_transfer(count=200, size=20, reliable=False, jitter=80, seed=3)
calm = run_transfer(count=200, size=20, reliable=False, jitter=0, seed=3)
check(jit["rx"].get("tp.unrel_reordered", 0) > 0 and calm["rx"].get("tp.unrel_reordered", -1) == 0, "o jitter reordena segmentos do modo nao confiavel (%d reordenados com 80 ms, 0 sem jitter)" % jit["rx"].get("tp.unrel_reordered", 0))

b = next_ports()
dead = run_once(["-a", "1", "-p", str(b), "-n", "2=127.0.0.1:%d" % (b + 1), "-m", "send", "-d", "2", "-N", "1", "-w", "1", "-t", "50", "-R", "3"], timeout=20)
dst = parse_stats(dead.stdout)
check(dead.returncode == 3 and dst.get("tp.retransmissions") == 3 and dst.get("tp.timeouts") == 3, "com -R 3 o transporte retransmite exatamente 3 vezes antes de desistir")


section("estatisticas exatas do receptor")
b = next_ports()
node = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-m", "sink"])
time.sleep(0.3)
peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
peer.bind(("127.0.0.1", b))
for seq in (0, 3, 4, 10, 2):
    peer.sendto(forge_frame(1, 2, 2, 2, seq, bench_msg(seq, seq), flags=0x01), ("127.0.0.1", b + 1))
    time.sleep(0.05)
peer.close()
out, _ = stop(node)
st = parse_stats(out)
check(st.get("tp.unrel_gaps") == 7 and st.get("tp.unrel_reordered") == 1 and st.get("tp.delivered") == 5, "modo nao confiavel com sequencias 0, 3, 4, 10 e 2: 7 lacunas (1 e 2, depois 5 a 9), 1 fora de ordem, 5 entregues")

b = next_ports()
node = start(["-a", "2", "-p", str(b + 1), "-n", "1=127.0.0.1:%d" % b, "-m", "sink"])
time.sleep(0.3)
peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
peer.bind(("127.0.0.1", b))
for seq, ident in enumerate((0, 1, 5, 6, 3)):
    peer.sendto(forge_frame(1, 2, 2, 2, seq, bench_msg(ident, ident)), ("127.0.0.1", b + 1))
    time.sleep(0.05)
peer.close()
out, _ = stop(node)
st = parse_stats(out)
check(st.get("app.bench_in_order") == 3 and st.get("app.bench_skipped") == 3 and st.get("app.bench_reordered") == 1 and st.get("app.bench_unique") == 5,
      "ids 0, 1, 5, 6 e 3: 3 em ordem, 3 puladas (2, 3 e 4), 1 fora de ordem e 5 unicas")


section("demonstracao automatica do encapsulamento")
demo = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "demo_chat.py")], capture_output=True, text=True, timeout=30)
flat = [" ".join(re.sub(r"^\[\s*[\d.]+ ms\]", "", line).split()) for line in demo.stdout.splitlines()]
send_lines = [
    'APP SEND chat TEXT "ola bob" (1 B de tipo + 7 B de texto) -> no 2 porta 1',
    "TRP SEND sport=1 dport=1 seq=1 ack=0 flags=DATA|REL cabecalho 13 B + carga 8 B",
    "NET SEND src=1 dst=2 ttl=16 proto=1 len=21 cabecalho 9 B + segmento 21 B, proximo salto 2",
    "LNK SEND len=30 crc=0xa597 cabecalho 4 B + carga 30 B = frame 34 B -> vizinho 2",
]
recv_lines = [
    "LNK RECV frame 30 B, CRC ok -> pacote de 26 B para a camada de rede",
    "NET RECV src=2 dst=1 ttl=16 proto=1 -> segmento de 17 B para o transporte",
    "TRP RECV sport=1 dport=1 seq=0 ack=0 flags=DATA|REL carga 4 B",
    'APP RECV chat HELLO_ACK "bob" de 2',
]
has = lambda block: any(flat[i:i + len(block)] == block for i in range(len(flat)))
check(demo.returncode == 0 and "[chat] sessao estabelecida com 2 (bob)" in demo.stdout and "[bob@2] oi alice" in demo.stdout, "demo_chat.py termina com sucesso e mostra as mensagens nos dois sentidos")
check(has(send_lines) and has(recv_lines), "o trace do demo_chat.py contem exatamente o trecho de envio (frame de 34 bytes) e de recepcao citado no relatorio")

conformance.run(check, section)

print("\nresultado: %d passaram, %d falharam" % (passed, failed))
if failed == 0:
    print("TODOS OS TESTES PASSARAM")
sys.exit(1 if failed else 0)
