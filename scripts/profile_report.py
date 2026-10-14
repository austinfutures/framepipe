#!/usr/bin/env python3
"""profile_report.py — explain latency spikes with scheduler traces.

    python3 scripts/profile_report.py results/<env>/profile-<tag>/<config>__<load>__<tool>

Reads the pipeline CSVs from the same run plus either
  timehist_fp.txt  (perf sched timehist, fp_ threads only)   or
  runq.txt         (scripts/runq_fp.bt output)
and writes profile.md + profile.png:
  * distribution of scheduling delay per fp_ thread (how long it was runnable but not running)
  * for bpftrace runs (same clock as our frames): the end-to-end latency timeline with
    every run-queue delay >= 200 µs of an fp_ thread marked, the share of the slowest 1%
    of frames that had such a delay inside their capture→inference window, and which
    tasks preempted us most often
Every number printed here is computed from the files in the directory.
"""
import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
INK, INK2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
SPIKE_US = 200
# Threads on the frame path. Main threads (fp_producer, fp_consumer) only set up and wait.
ROLE = {"fp_prod0": "stream 0 capture", "fp_prod1": "stream 1 capture", "fp_cons": "consumer + inference"}
STREAM0_PATH = ["fp_prod0", "fp_cons"]

# perf sched timehist line:  <time> [<cpu>] <task[tid/pid]> <wait ms> <sch delay ms> <run ms>
TIMEHIST = re.compile(r"^\s*(\d+\.\d+)\s+\[(\d+)\]\s+(\S+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)")


def style():
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": INK2, "axes.labelcolor": INK, "text.color": INK, "xtick.color": INK2,
        "ytick.color": INK2, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
        "axes.spines.top": False, "axes.spines.right": False, "font.size": 9, "legend.frameon": False,
    })


def thread_of(task: str) -> str:
    return task.split("[")[0]


def load_perf(d: Path) -> pd.DataFrame:
    rows = []
    for line in (d / "timehist_fp.txt").read_text().splitlines():
        m = TIMEHIST.match(line)
        if m and m.group(3).startswith("fp_"):
            rows.append({"thread": thread_of(m.group(3)), "cpu": int(m.group(2)),
                         "sched_delay_us": float(m.group(5)) * 1e3, "run_us": float(m.group(6)) * 1e3})
    return pd.DataFrame(rows)


def load_bpf(d: Path):
    runq, pre = [], []
    for line in (d / "runq.txt").read_text().splitlines():
        parts = line.split(",")
        if len(parts) != 7 or not parts[0].isdigit():
            continue  # histogram printout, headers, etc.
        ts, ev, comm, tid, cpu, delay, other = parts
        rec = {"ts_ns": int(ts), "thread": comm, "tid": int(tid), "cpu": int(cpu), "other": other}
        if ev == "runq":
            rec["sched_delay_us"] = float(delay)
            runq.append(rec)
        elif ev == "preempt":
            pre.append(rec)
    return pd.DataFrame(runq), pd.DataFrame(pre)


def describe(series: pd.Series) -> str:
    a = series.to_numpy(float)
    if a.size == 0:
        return "no events"
    return (f"n={a.size:,}  p50 {np.percentile(a, 50):,.1f}  p99 {np.percentile(a, 99):,.1f}  "
            f"max {a.max():,.1f} µs")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", type=Path)
    d = ap.parse_args().run_dir
    meta = json.loads((d / "meta.json").read_text())
    env = meta.get("env", {})
    title = f"{meta['config']} / {meta['load']}  |  env: {env.get('label', '?')}, kernel {env.get('kernel', '?')}"
    style()

    cons = pd.read_csv(d / "consumer.csv")
    proc = cons[(cons.status == 0) & (cons.stream == 0)].sort_values("t_capture").copy()
    proc["e2e_us"] = (proc.t_infer_end - proc.t_capture) / 1e3

    md = [f"# Scheduler profile — {title}", ""]
    if (d / "timehist_fp.txt").exists():
        tool = "perf sched timehist"
        ev = load_perf(d)
        pre = pd.DataFrame()
    elif (d / "runq.txt").exists():
        tool = "bpftrace runq_fp.bt"
        ev, pre = load_bpf(d)
    else:
        sys.exit("no timehist_fp.txt or runq.txt in " + str(d))
    if len(ev) == 0:
        err = d / "bpftrace.err"
        hint = err.read_text().strip()[-400:] if err.exists() else "(no bpftrace.err)"
        sys.exit(f"!! {tool} recorded no events for fp_ threads in {d}\n   tracer stderr: {hint}")

    # Only count what happened while frames were flowing (bpftrace events carry
    # CLOCK_MONOTONIC timestamps): excludes model load / warm-up, when the consumer is
    # still SCHED_OTHER, and the producer's start-up polling.
    has_time = "ts_ns" in ev.columns
    if has_time and len(proc):
        lo, hi = int(proc.t_capture.min()), int(proc.t_infer_end.max())
        ev = ev[(ev.ts_ns >= lo) & (ev.ts_ns <= hi)]
        if len(pre):
            pre = pre[(pre.ts_ns >= lo) & (pre.ts_ns <= hi)]
        md += [f"Window: first frame capture → last inference end ({(hi - lo) / 1e9:.1f} s); "
               "events outside it (start-up, warm-up) are excluded.", ""]

    md += [f"Tool: {tool}. Scheduling delay = time a thread was runnable but waiting for a CPU.", "",
           "| thread | role | scheduling delay |", "|---|---|---|"]
    threads = sorted(ev.thread.unique())
    for t in threads:
        md.append(f"| {t} | {ROLE.get(t, 'main/setup thread, not in the frame path')} | "
                  f"{describe(ev.loc[ev.thread == t, 'sched_delay_us'])} |")
    md.append("")

    fig, axes = plt.subplots(1, 2 if has_time else 1, figsize=(12 if has_time else 6, 3.8), squeeze=False)
    ax = axes[0][0]
    plotted = 0
    for i, t in enumerate([t for t in threads if t in ROLE]):
        v = np.sort(ev.loc[ev.thread == t, "sched_delay_us"].to_numpy(float))
        if v.size:
            ax.plot(np.maximum(v, 0.1), 1 - np.arange(v.size) / v.size, color=SERIES[i % len(SERIES)], label=t,
                    linewidth=1.6)
            plotted += 1
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("scheduling delay (µs, log)")
    ax.set_ylabel("fraction of wake-ups slower than x")
    if plotted:
        ax.legend()

    if has_time:
        ax = axes[0][1]
        t0 = proc.t_capture.min()
        ax.plot((proc.t_capture - t0) / 1e9, proc.e2e_us, color=SERIES[0], linewidth=0.8, label="frame e2e latency")
        spikes = ev[(ev.sched_delay_us >= SPIKE_US) & ev.thread.isin(STREAM0_PATH)]
        for i, t in enumerate(STREAM0_PATH):
            s = spikes[spikes.thread == t]
            if len(s):
                ax.scatter((s.ts_ns - t0) / 1e9, s.sched_delay_us, s=14, color=SERIES[(i + 1) % len(SERIES)],
                           label=f"{t} run-queue delay ≥ {SPIKE_US} µs", zorder=3)
        ax.set_yscale("log")
        ax.set_xlabel("time since first frame (s)")
        ax.set_ylabel("µs (log)")
        ax.legend(loc="upper right", fontsize=8)

        # Of the slowest 1% of stream-0 frames, how many overlap a run-queue wait >= SPIKE_US
        # of a thread on THAT frame's path (fp_prod0 or fp_cons)? A wait is the interval
        # [ts - delay, ts]; a frame is [t_capture, t_infer_end].
        if len(proc):
            cut = np.percentile(proc.e2e_us, 99)
            slow = proc[proc.e2e_us >= cut]
            w_end = spikes.ts_ns.to_numpy(np.int64)
            w_start = w_end - (spikes.sched_delay_us.to_numpy(float) * 1e3).astype(np.int64)
            hit = sum(bool(np.any((w_end >= a) & (w_start <= b)))
                      for a, b in zip(slow.t_capture.to_numpy(np.int64), slow.t_infer_end.to_numpy(np.int64)))
            md += [f"Slowest 1% of stream-0 frames (e2e ≥ {cut:,.0f} µs): {hit} of {len(slow)} overlap a run-queue "
                   f"wait ≥ {SPIKE_US} µs of fp_prod0 or fp_cons. The other {len(slow) - hit} are not explained "
                   "by the guest scheduler.", ""]
        if len(pre):
            n_tr = int((pre.other == "bpftrace").sum())
            md += [f"Observer effect: {n_tr} of {len(pre)} preemptions of our threads were by the tracer itself.", ""]
            top = pre.groupby(["thread", "other"]).size().sort_values(ascending=False).head(10)
            md += ["Who preempted our threads (count):", "", "| our thread | preempted by | count |", "|---|---|---:|"]
            md += [f"| {t} | {o} | {n} |" for (t, o), n in top.items()]
            md.append("")
    fig.suptitle(title, fontsize=9)
    fig.tight_layout()
    fig.savefig(d / "profile.png", dpi=130)
    plt.close(fig)

    (d / "profile.md").write_text("\n".join(md))
    print("\n".join(md))


if __name__ == "__main__":
    main()
