#!/usr/bin/env python3
"""analyze.py — turn a results/<env>/<tag>/ directory into numbers and plots.

    python3 scripts/analyze.py results/<env>/<tag>

Per run (each <config>__<load>__r<rep>/ directory):
  latency.png         histogram + latency-over-time of end-to-end latency

For the whole tag directory:
  summary.csv         one row per run, every metric
  results.md          p50/p99/p99.9/max tables per load + each fix's contribution
  fig_ladder.png      p99 and max end-to-end latency per config, idle vs stress
  fig_tail_<load>.png tail (1 - CDF) of end-to-end latency for the ladder configs
  fig_stages.png      median time per pipeline stage, per config
  fig_wakeup.png      producer wake-up lateness (scheduler jitter at the "camera")

Definitions (all from CLOCK_MONOTONIC timestamps, reported in microseconds):
  end-to-end     t_infer_end - t_capture          (stream 0, processed frames)
  wake-up late   t_capture - t_deadline           (producer thread woke this late)
  period jitter  (t_capture[k] - t_capture[k-1]) - 1e6/fps, consecutive frames only
  skew           t_capture(stream 1, k) - t_capture(stream 0, k)
  drops          missed deadlines + ring-full drops + consumer skips + frames lost in transit
Every number in the output comes from the CSVs; nothing is estimated.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib
import matplotlib.ticker

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# ---------------------------------------------------------------- style
# Categorical slots in fixed order (validated colorblind-safe palette, light mode).
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
INK, INK2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
LOAD_COLOR = {"idle": SERIES[0], "stress": SERIES[1]}

LADDER = ["baseline", "zerocopy", "fifo", "pin", "isolate", "mlock"]
LADDER_LABEL = {
    "baseline": "baseline (default sched, copy)",
    "zerocopy": "+ zero-copy",
    "fifo": "+ SCHED_FIFO",
    "pin": "+ CPU pinning",
    "isolate": "+ isolation",
    "mlock": "+ mlockall (all on)",
}
# "all on minus X" runs. "isolate" is exactly all-on minus mlockall, so it is reused.
ABLATION = {
    "no_zerocopy": "zero-copy",
    "no_fifo": "SCHED_FIFO",
    "no_isolate": "isolation",
    "no_pin_isolate": "pinning + isolation",
    "isolate": "mlockall",
}
ALL_ON = "mlock"


def style() -> None:
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": INK2, "axes.labelcolor": INK, "text.color": INK,
        "xtick.color": INK2, "ytick.color": INK2,
        "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
        "axes.spines.top": False, "axes.spines.right": False,
        "font.size": 9, "axes.titlesize": 10, "lines.linewidth": 2, "legend.frameon": False,
    })


# ---------------------------------------------------------------- stats
def q(a) -> dict:
    a = np.asarray(a, dtype=float)
    a = a[~np.isnan(a)]
    if a.size == 0:
        return {k: np.nan for k in ("n", "p50", "p99", "p999", "max", "mean", "std")}
    return {"n": a.size, "p50": np.percentile(a, 50), "p99": np.percentile(a, 99),
            "p999": np.percentile(a, 99.9), "max": a.max(), "mean": a.mean(), "std": a.std()}


def flat(prefix: str, d: dict) -> dict:
    return {f"{prefix}_{k}": v for k, v in d.items()}


def env_title(meta: dict) -> str:
    e = meta.get("env", {})
    return (f"env: {e.get('label', '?')} | kernel {e.get('kernel', '?')} | {e.get('nproc', '?')} CPUs"
            f" | {e.get('cpu_model', '')}")


# ---------------------------------------------------------------- one run
def analyze_run(d: Path):
    meta = json.loads((d / "meta.json").read_text())
    prod = pd.read_csv(d / "producer.csv")
    cons = pd.read_csv(d / "consumer.csv")
    fps = float(meta.get("fps", 60))
    period_us = 1e6 / fps
    has_model = meta.get("model", "none") != "none"

    t_start = prod.loc[prod.t_capture > 0, "t_capture"].min()
    proc = cons[(cons.status == 0) & (cons.stream == 0)].sort_values("seq")
    end = proc.t_infer_end if has_model else proc.t_dequeue
    e2e = (end - proc.t_capture) / 1e3
    stages = {
        "capture_to_enqueue": (proc.t_enqueue - proc.t_capture) / 1e3,  # render (+ copy in copy mode)
        "transport": (proc.t_dequeue - proc.t_enqueue) / 1e3,           # ring + futex wake-up
        "preprocess": ((proc.t_infer_start - proc.t_dequeue) / 1e3) if has_model else pd.Series(dtype=float),
        "inference": ((proc.t_infer_end - proc.t_infer_start) / 1e3) if has_model else pd.Series(dtype=float),
    }

    sched = prod[(prod.t_deadline > 0) & (prod.t_capture > 0)]
    wake = (sched.t_capture - sched.t_deadline) / 1e3

    jit = []
    for _, g in prod[prod.status != 2].groupby("stream"):
        g = g.sort_values("seq")
        consecutive = g.seq.diff() == 1
        jit.append(((g.t_capture.diff() / 1e3) - period_us)[consecutive].to_numpy())
    jit = np.concatenate(jit) if jit else np.array([])

    missed = int((prod.status == 2).sum())
    ring_full = int((prod.status == 1).sum())
    skipped = int((cons.status == 1).sum())
    ok = prod[prod.status == 0]
    lost = len(set(zip(ok.stream, ok.seq)) - set(zip(cons.stream, cons.seq)))
    drops = missed + ring_full + skipped + lost

    cap = prod[prod.status != 2].pivot_table(index="seq", columns="stream", values="t_capture")
    skew = ((cap[1] - cap[0]) / 1e3).dropna() if {0, 1} <= set(cap.columns) else pd.Series(dtype=float)

    row = {
        "run": d.name, "config": meta["config"], "load": meta["load"], "rep": meta["rep"],
        "transport": meta["transport"], "fifo": meta["fifo"], "pin": meta["pin"],
        "isolate": meta["isolate"], "mlock": meta["mlock"], "env": meta.get("env", {}).get("label", "?"),
        "frames_scheduled": len(prod), "drops": drops, "drop_missed_deadline": missed,
        "drop_ring_full": ring_full, "drop_consumer_skip": skipped, "drop_lost": lost,
        "drop_rate": drops / max(len(prod), 1),
        **flat("e2e", q(e2e)), **flat("wake", q(wake)), **flat("jitter_abs", q(np.abs(jit))),
        **flat("skew_abs", q(np.abs(skew))), "skew_mean": float(skew.mean()) if len(skew) else np.nan,
    }
    for k, v in stages.items():
        row.update(flat(k, q(v)))

    series = {"e2e": e2e.to_numpy(), "t_s": ((proc.t_capture - t_start) / 1e9).to_numpy()}
    plot_run(d, meta, row, series)
    return row, series, meta


def plot_run(d: Path, meta: dict, row: dict, s: dict) -> None:
    if len(s["e2e"]) == 0:
        return
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 3.4), gridspec_kw={"width_ratios": [1, 1.6]})
    a1.hist(s["e2e"], bins=80, color=SERIES[0], edgecolor=SURFACE, linewidth=0.3)
    a1.set_yscale("log")
    a1.set_xlabel("end-to-end latency (µs)")
    a1.set_ylabel("frames")
    for k, lbl in (("e2e_p50", "p50"), ("e2e_p99", "p99")):
        a1.axvline(row[k], color=INK2, linewidth=1, linestyle="--")
        a1.annotate(f"{lbl} {row[k]:,.0f}", (row[k], a1.get_ylim()[1]), rotation=90, va="top", ha="right",
                    fontsize=8, color=INK2)
    a2.plot(s["t_s"], s["e2e"], color=SERIES[0], linewidth=0.8)
    a2.set_xlabel("time since start (s)")
    a2.set_ylabel("end-to-end latency (µs)")
    fig.suptitle(f"{meta['config']} / {meta['load']} / rep {meta['rep']}    "
                 f"p50 {row['e2e_p50']:,.0f}  p99 {row['e2e_p99']:,.0f}  max {row['e2e_max']:,.0f} µs"
                 f"   drops {row['drops']}\n{env_title(meta)}", fontsize=9)
    fig.tight_layout()
    fig.savefig(d / "latency.png", dpi=130)
    plt.close(fig)


# ---------------------------------------------------------------- tables
def fmt(v, digits=0) -> str:
    if v is None or (isinstance(v, float) and np.isnan(v)):
        return "–"
    return f"{v:,.{digits}f}"


def fmt_delta(v) -> str:
    if v is None or np.isnan(v):
        return "–"
    return f"{v:+,.0f}"


def results_md(agg: pd.DataFrame, meta0: dict, n_reps: int) -> str:
    e = meta0.get("env", {})
    lines = [
        f"# Results — environment `{e.get('label', '?')}`",
        "",
        f"Kernel `{e.get('kernel', '?')}` ({e.get('preempt', '?')}), {e.get('nproc', '?')} CPUs "
        f"({e.get('cpu_model', '?')}), clocksource `{e.get('clocksrc', '?')}`. "
        f"{meta0.get('duration')} s per run at {meta0.get('fps')} Hz, model `{meta0.get('model')}`, "
        f"{n_reps} rep(s) per cell (median across reps). The exact stress-ng command is in each "
        f"`*__stress__*/meta.json`; the exact pipeline commands are in `cmd.txt`.",
        "",
        "All latencies in µs. e2e = capture → inference end (stream 0). wake = producer wake-up lateness. "
        "|jitter| = |frame period − 16,667 µs|. skew = |capture time difference| between the two streams.",
        "",
    ]
    order = [c for c in LADDER + list(ABLATION) if c in agg.index.get_level_values(0)]
    order = list(dict.fromkeys(order))
    for load in [l for l in ("idle", "stress") if l in agg.index.get_level_values(1)]:
        lines += [f"## {load}", "",
                  "| config | e2e p50 | e2e p99 | e2e p99.9 | e2e max | wake p99 | wake max | jitter p99 (abs) | drops | skew p99 |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for c in order:
            if (c, load) not in agg.index:
                continue
            r = agg.loc[(c, load)]
            name = LADDER_LABEL.get(c, f"all on − {ABLATION.get(c, c)}" if c.startswith("no_") else c)
            lines.append(f"| {name} | {fmt(r.e2e_p50)} | {fmt(r.e2e_p99)} | {fmt(r.e2e_p999)} | {fmt(r.e2e_max)} | "
                         f"{fmt(r.wake_p99)} | {fmt(r.wake_max)} | {fmt(r.jitter_abs_p99)} | {fmt(r.drops)} | "
                         f"{fmt(r.skew_abs_p99)} |")
        lines.append("")

    # contribution tables
    for load in [l for l in ("idle", "stress") if l in agg.index.get_level_values(1)]:
        rows = []
        prev = None
        for c in LADDER:
            if (c, load) not in agg.index:
                continue
            r = agg.loc[(c, load)]
            if prev is not None:
                rows.append(f"| {LADDER_LABEL[c]} | {fmt_delta(r.e2e_p99 - prev.e2e_p99)} | "
                            f"{fmt_delta(r.e2e_max - prev.e2e_max)} | {fmt_delta(r.wake_p99 - prev.wake_p99)} | "
                            f"{fmt_delta(r.wake_max - prev.wake_max)} |")
            prev = r
        if rows:
            lines += [f"## Contribution of each fix, cumulative ({load})", "",
                      "Change vs. the previous row of the ladder (negative = better).", "",
                      "| step | Δ e2e p99 | Δ e2e max | Δ wake p99 | Δ wake max |", "|---|---:|---:|---:|---:|", *rows, ""]
        if (ALL_ON, load) in agg.index:
            a = agg.loc[(ALL_ON, load)]
            rows = []
            for c, what in ABLATION.items():
                if (c, load) not in agg.index:
                    continue
                r = agg.loc[(c, load)]
                rows.append(f"| {what} | {fmt_delta(r.e2e_p99 - a.e2e_p99)} | {fmt_delta(r.e2e_max - a.e2e_max)} | "
                            f"{fmt_delta(r.wake_p99 - a.wake_p99)} | {fmt_delta(r.wake_max - a.wake_max)} |")
            if rows:
                lines += [f"## Contribution of each fix, ablation ({load})", "",
                          "Cost of removing one fix from the all-on configuration (positive = that fix was helping).", "",
                          "| removed | Δ e2e p99 | Δ e2e max | Δ wake p99 | Δ wake max |", "|---|---:|---:|---:|---:|",
                          *rows, ""]
    return "\n".join(lines)


# ---------------------------------------------------------------- combined figures
def bar_by_load(ax, agg, configs, metric, loads, ylabel):
    x = np.arange(len(configs))
    w = 0.8 / max(len(loads), 1)
    for i, load in enumerate(loads):
        vals = [agg.loc[(c, load), metric] if (c, load) in agg.index else np.nan for c in configs]
        ax.bar(x + (i - (len(loads) - 1) / 2) * w, vals, w * 0.92, label=load, color=LOAD_COLOR.get(load, SERIES[i]))
    ax.set_xticks(x)
    ax.set_xticklabels([LADDER_LABEL.get(c, c) for c in configs], rotation=25, ha="right")
    ax.set_ylabel(ylabel)
    ax.grid(axis="x", visible=False)


def dot_by_load(ax, agg, configs, metric, loads, ylabel):
    """Single-frame extremes (max): dots with the value printed, on a log axis.
    Bars would be misleading on a log scale (their length depends on where the axis starts)."""
    x = np.arange(len(configs))
    w = 0.8 / max(len(loads), 1)
    for i, load in enumerate(loads):
        xs = x + (i - (len(loads) - 1) / 2) * w
        vals = [agg.loc[(c, load), metric] if (c, load) in agg.index else np.nan for c in configs]
        ax.scatter(xs, vals, s=46, color=LOAD_COLOR.get(load, SERIES[i]), label=load, zorder=3,
                   edgecolor=SURFACE, linewidth=1.5)
        for xi, v in zip(xs, vals):
            if not np.isnan(v):
                ax.annotate(f"{v:,.0f}", (xi, v), textcoords="offset points", xytext=(0, 7), ha="center",
                            fontsize=7, color=INK2)
    ax.set_yscale("log")
    ax.yaxis.set_major_locator(matplotlib.ticker.LogLocator(base=10, subs=(1, 2, 5)))
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    ax.set_xticks(x)
    ax.set_xticklabels([LADDER_LABEL.get(c, c) for c in configs], rotation=25, ha="right")
    ax.set_xlim(-0.6, len(configs) - 0.4)
    ax.set_ylabel(ylabel)
    ax.grid(axis="x", visible=False)


def figures(out: Path, agg: pd.DataFrame, series: dict, meta0: dict) -> None:
    title = env_title(meta0)
    loads = [l for l in ("idle", "stress") if l in agg.index.get_level_values(1)]
    ladder = [c for c in LADDER if c in agg.index.get_level_values(0)]
    if not ladder:
        return

    fig, axes = plt.subplots(1, 2, figsize=(11, 4))
    bar_by_load(axes[0], agg, ladder, "e2e_p99", loads, "p99 end-to-end latency (µs)")
    dot_by_load(axes[1], agg, ladder, "e2e_max", loads, "max end-to-end latency (µs, log) — one frame each")
    axes[0].legend(title="load")
    fig.suptitle(f"End-to-end latency by configuration\n{title}", fontsize=9)
    fig.tight_layout()
    fig.savefig(out / "fig_ladder.png", dpi=130)
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(11, 4))
    bar_by_load(axes[0], agg, ladder, "wake_p99", loads, "p99 producer wake-up lateness (µs)")
    dot_by_load(axes[1], agg, ladder, "wake_max", loads, "max wake-up lateness (µs, log) — one frame each")
    axes[0].legend(title="load")
    fig.suptitle(f"Scheduler jitter at the frame source\n{title}", fontsize=9)
    fig.tight_layout()
    fig.savefig(out / "fig_wakeup.png", dpi=130)
    plt.close(fig)

    for load in loads:
        fig, ax = plt.subplots(figsize=(7.5, 4.2))
        for i, c in enumerate(ladder):
            v = series.get((c, load))
            if v is None or len(v) == 0:
                continue
            v = np.sort(v)
            tail = 1.0 - np.arange(len(v)) / len(v)
            ax.plot(v, tail, color=SERIES[i], label=LADDER_LABEL[c], linewidth=1.6)
        ax.set_yscale("log")
        ax.set_xscale("log")
        ax.set_xlabel("end-to-end latency (µs, log)")
        ax.set_ylabel("fraction of frames slower than x (log)")
        ax.legend(loc="lower left")
        fig.suptitle(f"Latency tail under {load} load (lower-left is better)\n{title}", fontsize=9)
        fig.tight_layout()
        fig.savefig(out / f"fig_tail_{load}.png", dpi=130)
        plt.close(fig)

    load = "stress" if "stress" in loads else loads[0]
    stage_cols = [("capture_to_enqueue_p50", "capture → enqueue"), ("transport_p50", "transport"),
                  ("preprocess_p50", "preprocess"), ("inference_p50", "inference")]
    fig, ax = plt.subplots(figsize=(8, 3.6))
    y = np.arange(len(ladder))
    left = np.zeros(len(ladder))
    for i, (col, lbl) in enumerate(stage_cols):
        vals = np.array([agg.loc[(c, load), col] if (c, load) in agg.index else np.nan for c in ladder], float)
        vals = np.nan_to_num(vals)
        ax.barh(y, vals, left=left, color=SERIES[i], label=lbl, edgecolor=SURFACE, linewidth=2, height=0.6)
        left += vals
    ax.set_yticks(y)
    ax.set_yticklabels([LADDER_LABEL[c] for c in ladder])
    ax.invert_yaxis()
    ax.set_xlabel(f"median time per stage (µs), {load} load")
    ax.grid(axis="y", visible=False)
    ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.22))
    fig.suptitle(f"Where the time goes (median per stage)\n{title}", fontsize=9)
    fig.tight_layout()
    fig.savefig(out / "fig_stages.png", dpi=130)
    plt.close(fig)


# ---------------------------------------------------------------- main
def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("tag_dir", type=Path)
    args = ap.parse_args()
    out = args.tag_dir
    run_dirs = sorted(p for p in out.iterdir() if (p / "meta.json").exists() and (p / "consumer.csv").exists())
    if not run_dirs:
        sys.exit(f"no complete runs under {out}")
    style()

    rows, series_all, meta0 = [], {}, None
    for d in run_dirs:
        try:
            row, s, meta = analyze_run(d)
        except Exception as ex:  # a failed run shouldn't hide the others
            print(f"skip {d.name}: {ex}")
            continue
        rows.append(row)
        meta0 = meta0 or meta
        series_all.setdefault((row["config"], row["load"]), []).append(s["e2e"])
    df = pd.DataFrame(rows)
    df.to_csv(out / "summary.csv", index=False)

    n_reps = int(df.groupby(["config", "load"]).size().max())
    agg = df.groupby(["config", "load"]).median(numeric_only=True)
    series = {k: np.concatenate(v) for k, v in series_all.items()}  # pooled across reps for tail plots

    md = results_md(agg, meta0, n_reps)
    (out / "results.md").write_text(md)
    figures(out, agg, series, meta0)
    print(md)
    print(f"\nwrote {out/'summary.csv'}, {out/'results.md'} and fig_*.png")


if __name__ == "__main__":
    main()
