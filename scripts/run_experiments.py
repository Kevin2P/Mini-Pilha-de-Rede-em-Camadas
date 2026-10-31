import csv
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import ROOT, run_transfer  # noqa: E402

RESULTS = os.path.join(ROOT, "results")
SEEDS = [1, 2, 3]
COLUMNS = ["experiment", "loss", "corrupt", "delay_ms", "window", "reliable", "verify", "size", "count", "seed",
           "completed", "elapsed_ms", "delivered_unique", "in_order", "bad_payload", "sent", "segments_tx",
           "retransmissions", "timeouts", "rtt_avg_ms", "frames_tx_total", "wire_bytes", "app_bytes", "crc_failures",
           "frames_lost_sim", "frames_corrupted_sim", "unrel_gaps"]


def row(name, params, r):
    tx, rx = r["tx"], r["rx"]
    samples = tx.get("tp.rtt_samples", 0)
    count = params["count"]
    completed = int(r["tx_rc"] == 0 and tx.get("node.bench_failed", 1) == 0)
    return {
        "experiment": name,
        "loss": params.get("loss", 0), "corrupt": params.get("corrupt", 0), "delay_ms": params.get("delay", 0),
        "window": params.get("window", 4), "reliable": int(params.get("reliable", True)),
        "verify": int(params.get("verify", True)), "size": params["size"], "count": count, "seed": params["seed"],
        "completed": completed,
        "elapsed_ms": round(tx.get("app.bench_elapsed_us", 0) / 1000.0, 3),
        "delivered_unique": rx.get("app.bench_unique", 0), "in_order": rx.get("app.bench_in_order", 0),
        "bad_payload": rx.get("app.bench_bad_payload", 0) + rx.get("app.bench_bad_format", 0),
        "sent": tx.get("app.bench_sent", 0), "segments_tx": tx.get("tp.segments_tx", 0),
        "retransmissions": tx.get("tp.retransmissions", 0), "timeouts": tx.get("tp.timeouts", 0),
        "rtt_avg_ms": round(tx.get("tp.rtt_sum_us", 0) / samples / 1000.0, 3) if samples else "",
        "frames_tx_total": tx.get("link.frames_tx", 0) + rx.get("link.frames_tx", 0),
        "wire_bytes": tx.get("link.bytes_tx", 0) + rx.get("link.bytes_tx", 0),
        "app_bytes": count * params["size"],
        "crc_failures": tx.get("link.rx_bad_checksum", 0) + rx.get("link.rx_bad_checksum", 0),
        "frames_lost_sim": tx.get("link.frames_lost_sim", 0) + rx.get("link.frames_lost_sim", 0),
        "frames_corrupted_sim": tx.get("link.frames_corrupted_sim", 0) + rx.get("link.frames_corrupted_sim", 0),
        "unrel_gaps": rx.get("tp.unrel_gaps", 0),
    }


def run_grid(name, grid):
    os.makedirs(RESULTS, exist_ok=True)
    path = os.path.join(RESULTS, name + ".csv")
    t0 = time.time()
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS)
        w.writeheader()
        for i, params in enumerate(grid, 1):
            r = run_transfer(**params)
            w.writerow(row(name, params, r))
            f.flush()
            print("  %s %d/%d %s" % (name, i, len(grid), {k: v for k, v in params.items() if k not in ("count", "retries")}), flush=True)
    print("%s: %d execucoes em %.1f s -> %s" % (name, len(grid), time.time() - t0, os.path.relpath(path, ROOT)))


def exp_loss():
    return [dict(count=100, size=100, loss=l, delay=5, window=4, rto=60, retries=40, seed=s)
            for l in (0.0, 0.05, 0.10, 0.20, 0.30) for s in SEEDS]


def exp_delay():
    return [dict(count=50, size=100, loss=0.0, delay=d, window=w, rto=4 * d + 60, retries=40, seed=1)
            for d in (0, 5, 10, 20, 50) for w in (1, 4, 16)]


def exp_corruption():
    return [dict(count=100, size=100, corrupt=c, verify=v, delay=2, window=4, rto=60, retries=20, seed=s, wall=40)
            for c in (0.0, 0.05, 0.10, 0.20) for v in (True, False) for s in SEEDS]


def exp_overhead():
    return [dict(count=50, size=sz, reliable=rel, loss=0.0, delay=0, window=4, rto=100, retries=20, seed=1)
            for sz in (5, 10, 50, 100, 250, 500, 1000) for rel in (True, False)]


def exp_reliability():
    return [dict(count=100, size=100, reliable=rel, loss=l, delay=5, window=4, rto=60, retries=40, seed=s)
            for l in (0.0, 0.10, 0.20, 0.30) for rel in (True, False) for s in SEEDS]


def exp_window():
    return [dict(count=60, size=100, loss=l, delay=d, window=w, rto=rto, retries=80, seed=s)
            for (l, d, rto) in ((0.0, 20, 140), (0.1, 20, 140), (0.3, 5, 60))
            for w in (1, 4, 16) for s in SEEDS]


def exp_window_sweep():
    return [dict(count=300, size=100, loss=0.05, delay=1, window=w, rto=80, retries=80, seed=s)
            for w in (1, 2, 4, 8, 16, 32, 64) for s in SEEDS]


EXPERIMENTS = {"loss": exp_loss, "delay": exp_delay, "corruption": exp_corruption,
               "overhead": exp_overhead, "reliability": exp_reliability, "window": exp_window, "window_sweep": exp_window_sweep}

if __name__ == "__main__":
    names = sys.argv[1:] or list(EXPERIMENTS)
    for n in names:
        run_grid(n, EXPERIMENTS[n]())
