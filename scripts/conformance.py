import socket
import struct
import time

from harness import crc16, next_ports, parse_stats, start, stop

HOST = "127.0.0.1"
FLAG_DATA, FLAG_ACK, FLAG_REL = 0x01, 0x02, 0x04


def decode(datagram):
    out = {"ok": False}
    if len(datagram) < 4 + 9 + 13:
        return out
    length, crc = struct.unpack(">HH", datagram[:4])
    out["link_len_ok"] = length == len(datagram) - 4
    out["crc_ok"] = crc == crc16(datagram[:2] + b"\x00\x00" + datagram[4:])
    packet = datagram[4:]
    version, ttl, src, dst, proto, plen = struct.unpack(">BBHHBH", packet[:9])
    out.update(version=version, ttl=ttl, src=src, dst=dst, proto=proto, net_len_ok=plen == len(packet) - 9)
    segment = packet[9:]
    sport, dport, seq, ack, flags = struct.unpack(">HHIIB", segment[:13])
    out.update(sport=sport, dport=dport, seq=seq, ack=ack, flags=flags, payload=segment[13:], raw=datagram)
    out["ok"] = out["link_len_ok"] and out["crc_ok"] and out["net_len_ok"]
    return out


def pattern(ident, i):
    return ((ident ^ (ident >> 8) ^ (ident >> 16) ^ (ident >> 24)) + i) & 0xFF


def frame(src, dst, sport, dport, seq, ack, flags, payload=b""):
    segment = struct.pack(">HHIIB", sport, dport, seq, ack, flags) + payload
    packet = struct.pack(">BBHHBH", 1, 16, src, dst, 1, len(segment)) + segment
    data = bytearray(struct.pack(">HH", len(packet), 0) + packet)
    data[2:4] = struct.pack(">H", crc16(bytes(data)))
    return bytes(data)


def collect(sock, duration):
    frames = []
    end = time.time() + duration
    while time.time() < end:
        sock.settimeout(max(0.01, end - time.time()))
        try:
            data, _ = sock.recvfrom(4096)
        except socket.timeout:
            break
        frames.append(decode(data))
    return frames


def run(check, section):
    section("conformidade independente com o formato documentado no README")
    base = next_ports()
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind((HOST, base + 1))
    node = start(["-a", "1", "-p", str(base), "-n", "2=%s:%d" % (HOST, base + 1), "-m", "send", "-d", "2",
                  "-N", "8", "-s", "20", "-w", "3", "-t", "300", "-R", "5"])
    first = collect(peer, 0.15)
    check(len(first) == 3 and all(f["ok"] for f in first), "o emissor com janela 3 transmite exatamente 3 segmentos antes do primeiro ACK, com enlace, rede e CRC corretos")
    h = first[0] if first else {}
    check(h.get("version") == 1 and h.get("ttl") == 16 and h.get("src") == 1 and h.get("dst") == 2 and h.get("proto") == 1,
          "cabecalho de rede: versao 1, TTL 16, origem 1, destino 2, protocolo 1")
    check(all(f["flags"] == FLAG_DATA | FLAG_REL and f["sport"] == 2 and f["dport"] == 2 and f["ack"] == 0 for f in first) and [f["seq"] for f in first] == [0, 1, 2],
          "cabecalho de transporte: DATA|REL, portas 2 e 2, sequencias 0, 1 e 2")
    bench_ok = all(f["payload"][0] == 10 and struct.unpack(">I", f["payload"][1:5])[0] == f["seq"] and len(f["payload"]) == 20 and
                   all(f["payload"][5 + i] == pattern(f["seq"], i) for i in range(15)) for f in first)
    check(bench_ok, "mensagem de benchmark: tipo 10, identificador de 4 bytes e bytes de padrao conforme a formula")
    again = []
    for _ in range(4):
        peer.sendto(frame(2, 1, 2, 2, 0, 0, FLAG_ACK), (HOST, base))
        again += collect(peer, 0.1)
    resent = [f for f in again if f["flags"] == FLAG_DATA | FLAG_REL]
    check(len(resent) >= 3 and [f["seq"] for f in resent[:3]] == [0, 1, 2] and all(resent[i]["raw"] == first[i]["raw"] for i in range(3)),
          "ao expirar o temporizador, go-back-N reenvia os 3 segmentos pendentes com os mesmos bytes, mesmo sob ACKs duplicados que nao reiniciam o temporizador")
    peer.sendto(frame(2, 1, 2, 2, 0, 100, FLAG_ACK), (HOST, base))
    peer.sendto(frame(2, 1, 2, 2, 0, 2, FLAG_ACK), (HOST, base))
    slide = collect(peer, 0.12)
    seqs = [f["seq"] for f in slide if f["flags"] == FLAG_DATA | FLAG_REL]
    check(3 in seqs and 4 in seqs and 5 not in seqs,
          "o ACK cumulativo 2 confirma as sequencias 0 e 1 e a janela de 3 avanca para enviar 3 e 4, sem chegar ao 5 (um ACK de 100, alem do que esta em voo, e ignorado)")
    peer.sendto(frame(2, 1, 2, 2, 0, 5, FLAG_ACK), (HOST, base))
    slide = collect(peer, 0.12)
    seqs = [f["seq"] for f in slide if f["flags"] == FLAG_DATA | FLAG_REL]
    check(5 in seqs and 6 in seqs and 7 in seqs, "o ACK cumulativo 5 libera os segmentos 5, 6 e 7")
    peer.sendto(frame(2, 1, 2, 2, 0, 8, FLAG_ACK), (HOST, base))
    try:
        out, _ = node.communicate(timeout=3)
        rc = node.returncode
    except Exception:
        out, _ = stop(node)
        rc = None
    st = parse_stats(out)
    check(rc == 0 and st.get("app.bench_sent") == 8 and st.get("node.exit_code") == 0, "o ACK cumulativo 8 conclui a transferencia: o emissor termina sozinho com codigo 0")
    check(st.get("tp.dup_acks_rx") == 4 and st.get("tp.bad_acks") == 1 and st.get("tp.acks_rx") == 8,
          "contadores do emissor: 4 ACKs duplicados, 1 ACK invalido e 8 ACKs recebidos")
    check(st.get("tp.rtt_samples") == 2, "o RTT so e amostrado em segmentos nunca retransmitidos (2 amostras, nao 3)")
    peer.close()

    base = next_ports()
    prev, nxt = socket.socket(socket.AF_INET, socket.SOCK_DGRAM), socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    prev.bind((HOST, base))
    nxt.bind((HOST, base + 2))
    node = start(["-a", "2", "-p", str(base + 1), "-n", "1=%s:%d" % (HOST, base), "-n", "3=%s:%d" % (HOST, base + 2), "-m", "sink"])
    time.sleep(0.3)
    inner = tp_payload = bytes(range(40))
    prev.sendto(frame(1, 3, 7, 9, 5, 0, FLAG_DATA | FLAG_REL, inner), (HOST, base + 1))
    fwd = collect(nxt, 0.3)
    f0 = fwd[0] if fwd else {"ok": False}
    check(len(fwd) == 1 and f0["ok"] and f0["ttl"] == 15 and f0["src"] == 1 and f0["dst"] == 3 and f0["payload"] == inner and f0["seq"] == 5,
          "o no intermediario encaminha o pacote com TTL decrementado em 1, origem e destino preservados, carga intacta e CRC refeito")
    stop(node)
    prev.close()
    nxt.close()

    base = next_ports()
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind((HOST, base + 1))
    node = start(["-a", "1", "-p", str(base), "-n", "2=%s:%d" % (HOST, base + 1), "-m", "send", "-d", "2", "-N", "3", "-s", "20", "-w", "3", "-t", "400", "-R", "5"])
    collect(peer, 0.3)
    peer.sendto(frame(2, 1, 2, 2, 0, 1, FLAG_ACK), (HOST, base))
    quiet = [f for f in collect(peer, 0.3) if f["flags"] == FLAG_DATA | FLAG_REL]
    check(len(quiet) == 0, "um ACK que confirma progresso reinicia o temporizador: sem retransmissao no intervalo em que o temporizador original teria expirado")
    peer.sendto(frame(2, 1, 2, 2, 0, 3, FLAG_ACK), (HOST, base))
    time.sleep(0.4)
    stop(node)
    peer.close()

    base = next_ports()
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind((HOST, base))
    node = start(["-a", "2", "-p", str(base + 1), "-n", "1=%s:%d" % (HOST, base), "-m", "sink"])
    time.sleep(0.3)

    def body(i):
        return bytes([10]) + struct.pack(">I", i) + bytes(pattern(i, k) for k in range(15))

    acks = []
    for seq in (0, 2, 1, 1, 2):
        peer.sendto(frame(1, 2, 2, 2, seq, 0, FLAG_DATA | FLAG_REL, body(seq)), (HOST, base + 1))
        got = collect(peer, 0.12)
        acks.append(got[0] if got else {"ok": False, "ack": None})
    check(all(a["ok"] and a["flags"] == FLAG_ACK and a["sport"] == 2 and a["dport"] == 2 for a in acks), "o receptor responde cada segmento com um ACK bem formado (flags ACK, portas trocadas)")
    check([a["ack"] for a in acks] == [1, 1, 2, 2, 3], "ACK cumulativo: 0 gera 1, lacuna (2) repete 1, 1 gera 2, duplicata repete 2, 2 gera 3")
    out, _ = stop(node)
    st = parse_stats(out)
    check(st.get("app.bench_unique") == 3 and st.get("app.bench_in_order") == 3 and st.get("tp.dup_rx") == 1 and st.get("tp.out_of_order_dropped") == 1,
          "o receptor entrega 3 mensagens em ordem, descarta 1 duplicata e 1 segmento fora de ordem")
    peer.close()

    base = next_ports()
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind((HOST, base + 1))
    node = start(["-a", "1", "-p", str(base), "-n", "2=%s:%d" % (HOST, base + 1), "-d", "2", "-U", "alice"], stdin=True)
    hello = collect(peer, 0.3)
    check(len(hello) >= 1 and hello[0]["ok"] and hello[0]["sport"] == 1 and hello[0]["dport"] == 1 and hello[0]["payload"] == bytes([1]) + b"alice",
          "o HELLO do chat usa as portas 1 e 1, tipo 1 e o nome 'alice' como texto")
    peer.sendto(frame(2, 1, 1, 1, 0, 1, FLAG_ACK), (HOST, base))
    peer.sendto(frame(2, 1, 1, 1, 0, 0, FLAG_DATA | FLAG_REL, bytes([2]) + b"pybob"), (HOST, base))
    reply = collect(peer, 0.2)
    check(any(f["flags"] == FLAG_ACK and f["ack"] == 1 for f in reply), "a mensagem HELLO_ACK recebida e confirmada com ACK 1")
    node.stdin.write("oi\n/quit\n")
    node.stdin.flush()
    sent = collect(peer, 0.6)
    texts = [f["payload"] for f in sent if f["flags"] == FLAG_DATA | FLAG_REL]
    check(texts[:1] == [bytes([3]) + b"oi"] and bytes([4]) in texts[1:2], "uma linha digitada vira TEXT (tipo 3) e /quit envia BYE (tipo 4), nesta ordem")
    stop(node)
    peer.close()
