import os
import random
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import BIN, crc16, next_ports  # noqa: E402

HOST = "127.0.0.1"
ITERATIONS = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1


def link_frame(packet, fix_crc=True):
    frame = bytearray(struct.pack(">HH", len(packet), 0) + packet)
    if fix_crc:
        frame[2:4] = struct.pack(">H", crc16(bytes(frame)))
    return bytes(frame)


def net_packet(dst, payload, ttl=16, src=1, proto=1, version=1, plen=None):
    return struct.pack(">BBHHBH", version, ttl, src, dst, proto, len(payload) if plen is None else plen) + payload


def tp_segment(sport, dport, seq, ack, flags, payload=b""):
    return struct.pack(">HHIIB", sport, dport, seq, ack, flags) + payload


def bench_pattern(ident, i):
    return ((ident ^ (ident >> 8) ^ (ident >> 16) ^ (ident >> 24)) + i) & 0xFF


class Fuzzer:
    def __init__(self, rng, src_addr, dst_addr):
        self.rng = rng
        self.src = src_addr
        self.dst = dst_addr
        self.seq = {1: 0, 2: 0}
        self.recent = []

    def blob(self, lo, hi):
        return bytes(self.rng.getrandbits(8) for _ in range(self.rng.randint(lo, hi)))

    def wrap(self, segment, dst=None):
        return link_frame(net_packet(self.dst if dst is None else dst, segment, src=self.src))

    def raw(self):
        return self.blob(0, 1600)

    def random_packet(self):
        return link_frame(self.blob(0, 1560))

    def net(self):
        r = self.rng
        dst = r.choice([self.dst, self.dst, 3, 7, r.getrandbits(16)])
        ttl = r.choice([0, 1, 2, 16, r.getrandbits(8)])
        proto = r.choice([1, 1, 0, r.getrandbits(8)])
        version = r.choice([1, 1, 1, r.getrandbits(8)])
        payload = self.blob(0, 1560)
        plen = None if r.random() < 0.8 else r.getrandbits(16)
        return link_frame(net_packet(dst, payload, ttl, r.getrandbits(16), proto, version, plen))

    def transport(self):
        r = self.rng
        ports = [1, 2, 3, r.getrandbits(16)]
        flags = r.choice([0x05, 0x01, 0x02, 0x04, 0x00, 0x03, 0x07, r.getrandbits(8)])
        seq = r.choice([0, 1, 2, 5, 1 << 31, 0xFFFFFFFF, r.getrandbits(32)])
        ack = r.choice([0, 1, 2, 3, 10, 1 << 31, 0xFFFFFFFF, r.getrandbits(32)])
        return self.wrap(tp_segment(r.choice(ports), r.choice(ports), seq, ack, flags, self.blob(0, 1100)))

    def chat(self):
        r = self.rng
        body = bytes([r.choice([1, 2, 3, 4, 0, 5, 255])]) + self.blob(0, 210)
        seq = self.seq[1]
        self.seq[1] += 1
        return self.wrap(tp_segment(1, 1, seq, 0, 0x05, body))

    def bench(self):
        r = self.rng
        ident = r.choice([0, 1, 2, r.getrandbits(8), r.getrandbits(32), (1 << 20) - 1, 1 << 20])
        size = r.randint(5, 1000)
        pattern = bytes(bench_pattern(ident, i) if r.random() < 0.9 else r.getrandbits(8) for i in range(size - 5))
        body = bytes([r.choice([10, 10, 10, 9, 11])]) + struct.pack(">I", ident) + pattern
        if r.random() < 0.1:
            body = body[:r.randint(0, 6)]
        seq = self.seq[2]
        self.seq[2] += 1
        return self.wrap(tp_segment(2, 2, seq, 0, 0x05, body))

    def ack(self):
        r = self.rng
        ack = r.choice([0, 1, 2, 3, 4, 5, 50, 1 << 31, 0xFFFFFFFF, r.getrandbits(32)])
        port = r.choice([1, 2])
        return self.wrap(tp_segment(port, port, 0, ack, 0x02, self.blob(0, 3) if r.random() < 0.2 else b""))

    def mutate(self):
        if not self.recent:
            return self.raw()
        r = self.rng
        frame = bytearray(r.choice(self.recent))
        for _ in range(r.randint(1, 4)):
            if frame:
                frame[r.randrange(len(frame))] = r.getrandbits(8)
        if r.random() < 0.7 and len(frame) >= 4:
            frame[2:4] = struct.pack(">H", crc16(bytes(frame[:2]) + b"\x00\x00" + bytes(frame[4:])))
        return bytes(frame)

    def next_frame(self, kinds):
        frame = self.rng.choices(kinds, weights=[w for _, w in kinds_weights(kinds)])[0]()
        self.recent.append(frame)
        if len(self.recent) > 64:
            self.recent.pop(0)
        return frame


def kinds_weights(kinds):
    return [(k, 1) for k in kinds]


def spawn(args, errfile, stdin=False):
    return subprocess.Popen([BIN] + args, stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=errfile, text=True, errors="replace")


def finish(proc, errfile, name, sent):
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
    try:
        out, _ = proc.communicate(timeout=15)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        rc = -9
    errfile.flush()
    errfile.seek(0)
    err = errfile.read()
    bad = [l for l in err.splitlines() if "ERROR: AddressSanitizer" in l or "runtime error" in l or "LeakSanitizer" in l]
    stats = {k: int(v) for k, v in re.findall(r"STAT (\S+)=(\d+)", out)}
    ok = rc in (0, 2) and not bad
    reach = "frames_rx=%d net.delivered=%d tp.delivered=%d bad_len=%d bad_crc=%d bad_hdr=%d fwd=%d" % (
        stats.get("link.frames_rx", 0), stats.get("net.delivered", 0), stats.get("tp.delivered", 0),
        stats.get("link.rx_bad_length", 0), stats.get("link.rx_bad_checksum", 0), stats.get("net.bad_header", 0), stats.get("net.forwarded", 0))
    print("%s %-30s enviados=%d saida=%s | %s" % ("ok  " if ok else "FALHA", name, sent, rc, reach))
    for l in bad[:3]:
        print("      " + l)
    return ok


def role_sink(rng, n):
    base = next_ports()
    p_fuzz, p_node, p_next = base, base + 1, base + 2
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((HOST, p_fuzz))
    dummy = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dummy.bind((HOST, p_next))
    err = tempfile.TemporaryFile("w+")
    proc = spawn(["-a", "2", "-p", str(p_node), "-n", "1=%s:%d" % (HOST, p_fuzz), "-n", "3=%s:%d" % (HOST, p_next), "-m", "sink", "-v"], err)
    time.sleep(0.4)
    fz = Fuzzer(rng, 1, 2)
    kinds = [fz.raw, fz.random_packet, fz.net, fz.net, fz.transport, fz.transport, fz.chat, fz.chat, fz.bench, fz.bench, fz.mutate, fz.mutate]
    for i in range(n):
        sock.sendto(fz.next_frame(kinds), (HOST, p_node))
        if i % 200 == 199:
            time.sleep(0.003)
    time.sleep(0.5)
    ok = finish(proc, err, "receptor com encaminhamento", n)
    sock.close()
    dummy.close()
    return ok


def role_chat(rng, n):
    base = next_ports()
    p_fuzz, p_node = base, base + 1
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((HOST, p_fuzz))
    err = tempfile.TemporaryFile("w+")
    proc = spawn(["-a", "1", "-p", str(p_node), "-n", "2=%s:%d" % (HOST, p_fuzz), "-d", "2", "-U", "alice", "-v", "-P", "-t", "20", "-R", "100000"], err, stdin=True)
    time.sleep(0.4)
    fz = Fuzzer(rng, 2, 1)
    kinds = [fz.ack, fz.ack, fz.ack, fz.chat, fz.chat, fz.transport, fz.net, fz.mutate, fz.raw]
    for i in range(n):
        sock.sendto(fz.next_frame(kinds), (HOST, p_node))
        if i % 50 == 0:
            line = rng.choice([b"ola\n", b"\xff\xfe\x00bin\n", b"x" * 700 + b"\n", b"a\r\n", b"/quit_not\n", b"\n", b"\x1b[31mcor\n"])
            try:
                proc.stdin.write(line.decode("latin-1"))
                proc.stdin.flush()
            except (BrokenPipeError, ValueError):
                pass
        if i % 100 == 99:
            time.sleep(0.003)
    time.sleep(0.5)
    ok = finish(proc, err, "iniciador de chat", n)
    sock.close()
    return ok


def role_sender(rng, n):
    base = next_ports()
    p_fuzz, p_node = base, base + 1
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((HOST, p_fuzz))
    sock.settimeout(0.001)
    err = tempfile.TemporaryFile("w+")
    proc = spawn(["-a", "1", "-p", str(p_node), "-n", "2=%s:%d" % (HOST, p_fuzz), "-m", "send", "-d", "2", "-N", "100000", "-s", "50",
                  "-w", "64", "-t", "30", "-R", "100000", "-W", "6", "-v"], err)
    fz = Fuzzer(rng, 2, 1)
    kinds = [fz.ack, fz.ack, fz.ack, fz.transport, fz.net, fz.mutate]
    sent = 0
    deadline = time.time() + 5
    while sent < n and time.time() < deadline:
        sock.sendto(fz.next_frame(kinds), (HOST, p_node))
        sent += 1
        if sent % 50 == 0:
            try:
                while True:
                    sock.recvfrom(4096)
            except (socket.timeout, BlockingIOError):
                pass
    ok = finish(proc, err, "emissor de benchmark", sent)
    sock.close()
    return ok


def main():
    rng = random.Random(SEED)
    print("fuzzing: %d quadros por papel, semente %d, binario %s" % (ITERATIONS, SEED, BIN))
    results = [role_sink(rng, ITERATIONS), role_chat(rng, ITERATIONS // 2), role_sender(rng, ITERATIONS // 2)]
    print("RESULTADO:", "nenhuma falha" if all(results) else "HA FALHAS")
    sys.exit(0 if all(results) else 1)


main()
