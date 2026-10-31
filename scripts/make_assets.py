import csv
import os
import statistics as st

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")
OUT = os.path.join(ROOT, "report", "generated")
os.makedirs(OUT, exist_ok=True)
plt.rcParams.update({"font.size": 9, "axes.grid": True, "grid.alpha": 0.3, "figure.dpi": 150})


def load(name):
    with open(os.path.join(RES, name + ".csv")) as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for k, v in r.items():
            if k != "experiment" and v != "":
                r[k] = float(v)
    return rows


def sel(rows, **kw):
    return [r for r in rows if all(r[k] == v for k, v in kw.items())]


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def sd(xs):
    return st.stdev(xs) if len(xs) > 1 else 0.0


def fmt(x, d=1):
    return ("%.*f" % (d, x)).replace(".", ",")


def fmt_g(x, d=3):
    return ("%.*f" % (d, x)).rstrip("0").rstrip(".").replace(".", ",")


def pm(xs, d=1):
    return "%s $\\pm$ %s" % (fmt(mean(xs), d), fmt(sd(xs), d))


def write_table(name, header, body, spec):
    with open(os.path.join(OUT, name + ".tex"), "w") as f:
        f.write("\\begin{tabular}{%s}\n\\toprule\n%s \\\\\n\\midrule\n" % (spec, " & ".join(header)))
        for line in body:
            f.write(" & ".join(line) + " \\\\\n")
        f.write("\\bottomrule\n\\end{tabular}\n")


def retx_rate(r):
    return 100.0 * r["retransmissions"] / r["segments_tx"] if r["segments_tx"] else 0.0


def loss_assets():
    rows = load("loss")
    levels = sorted({r["loss"] for r in rows})
    body, t, rr, tt = [], [], [], []
    for l in levels:
        rs = sel(rows, loss=l)
        times = [r["elapsed_ms"] for r in rs]
        rates = [retx_rate(r) for r in rs]
        t.append((mean(times), sd(times)))
        rr.append((mean(rates), sd(rates)))
        body.append(["%d\\%%" % round(l * 100), pm(times, 0), pm([r["retransmissions"] for r in rs], 0),
                     pm(rates, 1), fmt(mean([r["app_bytes"] / r["elapsed_ms"] for r in rs]) * 1000 / 1024, 1),
                     "%d/%d" % (sum(r["delivered_unique"] == r["count"] for r in rs), len(rs))])
    write_table("loss", ["Perda", "Tempo (ms)", "Retransmiss\\~oes", "Taxa de retransmiss\\~ao (\\%)", "Vaz\\~ao (KiB/s)", "Completas"],
                body, "rrrrrr")
    x = [l * 100 for l in levels]
    fig, ax = plt.subplots(1, 2, figsize=(6.4, 2.5))
    ax[0].errorbar(x, [a for a, _ in t], yerr=[b for _, b in t], marker="o", capsize=3)
    ax[0].set_xlabel("perda de frames (%)")
    ax[0].set_ylabel("tempo de conclus\u00e3o (ms)")
    ax[1].errorbar(x, [a for a, _ in rr], yerr=[b for _, b in rr], marker="o", color="tab:red", capsize=3)
    ax[1].set_xlabel("perda de frames (%)")
    ax[1].set_ylabel("taxa de retransmiss\u00e3o (%)")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_loss.pdf"))
    plt.close(fig)


def delay_assets():
    rows = load("delay")
    delays = sorted({r["delay_ms"] for r in rows})
    windows = sorted({r["window"] for r in rows})
    body = []
    for d in delays:
        line = ["%d" % d]
        for w in windows:
            r = sel(rows, delay_ms=d, window=w)[0]
            line.append(fmt(r["elapsed_ms"], 0))
        rtt = sel(rows, delay_ms=d, window=windows[0])[0]["rtt_avg_ms"]
        line.append(fmt(rtt, 1))
        line.append(fmt(2.0 * d, 0))
        body.append(line)
    write_table("delay", ["Atraso por salto (ms)"] + ["Tempo, janela %d (ms)" % w for w in windows] +
                ["RTT medido (ms)", "$2\\times$ atraso (ms)"], body, "r" * (len(windows) + 3))
    fig, ax = plt.subplots(figsize=(3.4, 2.5))
    for w in windows:
        ax.plot(delays, [sel(rows, delay_ms=d, window=w)[0]["elapsed_ms"] for d in delays], marker="o", label="janela %d" % w)
    ax.set_xlabel("atraso do enlace, por salto (ms)")
    ax.set_ylabel("tempo de conclus\u00e3o (ms)")
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_delay.pdf"))
    plt.close(fig)


def corruption_assets():
    rows = load("corruption")
    levels = sorted({r["corrupt"] for r in rows})
    body = []
    series = {1.0: [], 0.0: []}
    for c in levels:
        for v in (1.0, 0.0):
            rs = sel(rows, corrupt=c, verify=v)
            ok = [100.0 * r["delivered_unique"] / r["count"] for r in rs]
            bad = [r["bad_payload"] for r in rs]
            series[v].append(mean(ok))
            body.append(["%d\\%%" % round(c * 100), "com CRC" if v else "sem CRC", pm(ok, 1), pm(bad, 1),
                         pm([r["crc_failures"] for r in rs], 1), pm([r["retransmissions"] for r in rs], 1)])
    write_table("corruption", ["Corrup\\c{c}\\~ao", "Enlace", "\\'Integras (\\%)", "Adulteradas na app",
                               "Frames rejeitados (CRC)", "Retransmiss\\~oes"], body, "llrrrr")
    fig, ax = plt.subplots(figsize=(3.4, 2.5))
    ax.plot([c * 100 for c in levels], series[1.0], marker="o", label="com CRC-16")
    ax.plot([c * 100 for c in levels], series[0.0], marker="s", color="tab:red", label="sem CRC-16")
    ax.set_xlabel("corrup\u00e7\u00e3o de frames (%)")
    ax.set_ylabel("\u00edntegras entregues (%)")
    ax.set_ylim(70, 102)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_corruption.pdf"))
    plt.close(fig)


def overhead_assets():
    rows = load("overhead")
    sizes = sorted({r["size"] for r in rows})
    body, meas, theo = [], {1.0: [], 0.0: []}, {1.0: [], 0.0: []}
    for s in sizes:
        line = ["%d" % s]
        for rel in (1.0, 0.0):
            r = sel(rows, size=s, reliable=rel)[0]
            m = r["wire_bytes"] / r["app_bytes"]
            t = (s + 52.0) / s if rel else (s + 26.0) / s
            meas[rel].append(m)
            theo[rel].append(t)
            line += [fmt_g(m), fmt_g(t)]
        body.append(line)
    write_table("overhead", ["Mensagem (B)", "Confi\\'avel, medido", "Confi\\'avel, previsto", "N\\~ao confi\\'avel, medido",
                             "N\\~ao confi\\'avel, previsto"], body, "rrrrr")
    fig, ax = plt.subplots(figsize=(3.4, 2.5))
    ax.plot(sizes, meas[1.0], "o", label="confi\u00e1vel (medido)")
    ax.plot(sizes, theo[1.0], "-", color="tab:blue", alpha=0.6)
    ax.plot(sizes, meas[0.0], "s", color="tab:green", label="n\u00e3o confi\u00e1vel (medido)")
    ax.plot(sizes, theo[0.0], "-", color="tab:green", alpha=0.6)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("tamanho da mensagem da aplica\u00e7\u00e3o (bytes)")
    ax.set_ylabel("overhead (raz\u00e3o de bytes)")
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_overhead.pdf"))
    plt.close(fig)


def reliability_assets():
    rows = load("reliability")
    levels = sorted({r["loss"] for r in rows})
    body, deliv = [], {1.0: [], 0.0: []}
    for l in levels:
        for rel in (1.0, 0.0):
            rs = sel(rows, loss=l, reliable=rel)
            frac = [100.0 * r["delivered_unique"] / r["count"] for r in rs]
            deliv[rel].append(mean(frac))
            body.append(["%d\\%%" % round(l * 100), "confi\\'avel" if rel else "n\\~ao confi\\'avel", pm(frac, 1),
                         pm([r["elapsed_ms"] for r in rs], 0) if rel else "n.a.", pm([r["wire_bytes"] / 1024.0 for r in rs], 1),
                         pm([r["retransmissions"] for r in rs], 0)])
    write_table("reliability", ["Perda", "Transporte", "Entregues (\\%)", "Tempo (ms)", "Bytes no enlace (KiB)", "Retransmiss\\~oes"],
                body, "llrrrr")
    fig, ax = plt.subplots(figsize=(3.4, 2.5))
    ax.plot([l * 100 for l in levels], deliv[1.0], marker="o", label="confi\u00e1vel")
    ax.plot([l * 100 for l in levels], deliv[0.0], marker="s", color="tab:red", label="n\u00e3o confi\u00e1vel")
    ax.set_xlabel("perda de frames (%)")
    ax.set_ylabel("mensagens entregues (%)")
    ax.set_ylim(40, 103)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_reliability.pdf"))
    plt.close(fig)


def window_assets():
    rows = load("window")
    combos = sorted({(r["loss"], r["delay_ms"]) for r in rows})
    windows = sorted({r["window"] for r in rows})
    body = []
    for l, d in combos:
        base = mean([r["elapsed_ms"] for r in sel(rows, loss=l, delay_ms=d, window=windows[0])])
        for w in windows:
            rs = sel(rows, loss=l, delay_ms=d, window=w)
            t = mean([r["elapsed_ms"] for r in rs])
            body.append(["%d\\%%" % round(l * 100), "%d" % d, "%d" % w, pm([r["elapsed_ms"] for r in rs], 0),
                         fmt(base / t, 1), pm([retx_rate(r) for r in rs], 1)])
    write_table("window", ["Perda", "Atraso (ms)", "Janela", "Tempo (ms)", "Ganho sobre a janela 1", "Taxa de retransmiss\\~ao (\\%)"],
                body, "rrrrrr")


def window_sweep_assets():
    rows = load("window_sweep")
    windows = sorted({r["window"] for r in rows})
    t = [mean([r["elapsed_ms"] for r in sel(rows, window=w)]) for w in windows]
    tsd = [sd([r["elapsed_ms"] for r in sel(rows, window=w)]) for w in windows]
    rr = [mean([retx_rate(r) for r in sel(rows, window=w)]) for w in windows]
    fig, ax = plt.subplots(1, 2, figsize=(6.4, 2.5))
    ax[0].errorbar(windows, t, yerr=tsd, marker="o", capsize=3)
    ax[0].set_xscale("log", base=2)
    ax[0].set_xticks(windows)
    ax[0].set_xticklabels([str(int(w)) for w in windows])
    ax[0].set_xlabel("janela (segmentos)")
    ax[0].set_ylabel("tempo de conclus\u00e3o (ms)")
    ax[0].set_ylim(0, max(t) * 1.25)
    ax[1].plot(windows, rr, marker="o", color="tab:red")
    ax[1].set_xscale("log", base=2)
    ax[1].set_xticks(windows)
    ax[1].set_xticklabels([str(int(w)) for w in windows])
    ax[1].set_xlabel("janela (segmentos)")
    ax[1].set_ylabel("taxa de retransmiss\u00e3o (%)")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_window_sweep.pdf"))
    plt.close(fig)
    for w, a, b, c in zip(windows, t, tsd, rr):
        print("janela %2d: tempo %.0f +- %.0f ms, retransmissoes %.1f%%" % (w, a, b, c))


if __name__ == "__main__":
    window_sweep_assets()
    window_assets()
    loss_assets()
    delay_assets()
    corruption_assets()
    overhead_assets()
    reliability_assets()
    print("gerado:", ", ".join(sorted(os.listdir(OUT))))
