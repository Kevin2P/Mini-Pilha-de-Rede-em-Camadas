import itertools
import os
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("STACK_BIN", os.path.join(ROOT, "bin", "ministack"))
if not os.path.isabs(BIN):
    BIN = os.path.join(ROOT, BIN)

_ports = itertools.count(30000, 4)
_lock = threading.Lock()


def next_ports():
    with _lock:
        return next(_ports)


def parse_stats(text):
    stats = {}
    for line in text.splitlines():
        if line.startswith("STAT "):
            key, _, value = line[5:].partition("=")
            stats[key] = float(value) if "." in value else int(value)
    return stats


def link_args(loss, corrupt, delay, jitter, verify, seed):
    args = ["-l", str(loss), "-e", str(corrupt), "-D", str(delay), "-j", str(jitter), "-S", str(seed)]
    if not verify:
        args.append("-x")
    return args


def start(args, stdin=False):
    return subprocess.Popen(
        [BIN] + args,
        stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )


def stop(proc):
    proc.terminate()
    try:
        out, err = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
    return out, err


def run_transfer(count=100, size=100, reliable=True, loss=0.0, corrupt=0.0, delay=0, jitter=0,
                 verify=True, window=4, rto=100, retries=30, seed=1, relay=False, ttl=16,
                 chat_text=None, interval=0, wall=90):
    base = next_ports()
    p1, p2, p3 = base, base + 1, base + 2
    host = "127.0.0.1"
    transport = ["-w", str(window), "-t", str(rto), "-R", str(retries), "-T", str(ttl)]
    sim = lambda s: link_args(loss, corrupt, delay, jitter, verify, s)

    if relay:
        sink = start(["-a", "3", "-p", str(p3), "-n", "2=%s:%d" % (host, p2), "-r", "1=2", "-m", "sink"]
                     + sim(seed + 2000) + transport)
        mid = start(["-a", "2", "-p", str(p2), "-n", "1=%s:%d" % (host, p1), "-n", "3=%s:%d" % (host, p3), "-m", "sink"]
                    + sim(seed + 1000) + transport)
        dst, nb = 3, ["-n", "2=%s:%d" % (host, p1 + 1), "-r", "3=2"]
        nb = ["-n", "2=%s:%d" % (host, p2), "-r", "3=2"]
    else:
        mid = None
        sink = start(["-a", "2", "-p", str(p2), "-n", "1=%s:%d" % (host, p1), "-m", "sink"]
                     + sim(seed + 1000) + transport)
        dst, nb = 2, ["-n", "2=%s:%d" % (host, p2)]

    time.sleep(0.25)
    args = ["-a", "1", "-p", str(p1)] + nb + ["-m", "send", "-d", str(dst), "-N", str(count), "-s", str(size),
                                              "-i", str(interval), "-W", str(wall)] + sim(seed) + transport
    if not reliable:
        args.append("-u")
    if chat_text:
        args += ["-c", chat_text]
    sender = start(args)
    try:
        tx_out, tx_err = sender.communicate(timeout=wall + 10)
    except subprocess.TimeoutExpired:
        sender.kill()
        tx_out, tx_err = sender.communicate()
    time.sleep(0.2)
    rx_out, rx_err = stop(sink)
    result = {"tx": parse_stats(tx_out), "rx": parse_stats(rx_out), "tx_rc": sender.returncode,
              "tx_out": tx_out, "rx_out": rx_out, "tx_err": tx_err}
    if mid:
        mid_out, _ = stop(mid)
        result["relay"] = parse_stats(mid_out)
    return result


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def forge_frame(src, dst, src_port, dst_port, seq, body, flags=0x05):
    import struct
    segment = struct.pack(">HHIIB", src_port, dst_port, seq, 0, flags) + body
    packet = struct.pack(">BBHHBH", 1, 16, src, dst, 1, len(segment)) + segment
    frame = bytearray(struct.pack(">HH", len(packet), 0) + packet)
    frame[2:4] = struct.pack(">H", crc16(bytes(frame)))
    return bytes(frame)


def run_once(args, timeout=30):
    return subprocess.run([BIN] + args, capture_output=True, text=True, errors="replace",
                          timeout=timeout, stdin=subprocess.DEVNULL)
