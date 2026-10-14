#!/usr/bin/env python3
"""quick_stats.py — print the key numbers for one run directory (no plots).

    python3 scripts/quick_stats.py /tmp/m2          # producer.csv only (standalone) is fine
    python3 scripts/quick_stats.py results/<env>/<tag>/baseline__idle__r1

Use it during the early milestones, before a full experiment set exists.
"""
import sys
from pathlib import Path

import numpy as np
import pandas as pd


def line(name, a):
    a = np.asarray(a, float)
    if a.size == 0:
        print(f"  {name:<34} (no samples)")
        return
    print(f"  {name:<34} n={a.size:<6} p50={np.percentile(a, 50):>10,.1f}  p99={np.percentile(a, 99):>10,.1f}"
          f"  p99.9={np.percentile(a, 99.9):>10,.1f}  max={a.max():>10,.1f}  µs")


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    fps = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    period = 1e6 / fps
    prod = pd.read_csv(d / "producer.csv")
    print(f"producer ({len(prod)} scheduled frames): ok={int((prod.status == 0).sum())} "
          f"ring_full={int((prod.status == 1).sum())} missed_deadline={int((prod.status == 2).sum())}")
    s = prod[(prod.t_deadline > 0) & (prod.t_capture > 0)]
    line("wake-up lateness", (s.t_capture - s.t_deadline) / 1e3)
    jit = []
    for _, g in prod[prod.status != 2].groupby("stream"):
        g = g.sort_values("seq")
        jit.append((((g.t_capture.diff() / 1e3) - period)[g.seq.diff() == 1]).to_numpy())
    if jit:
        line("|period jitter|", np.abs(np.concatenate(jit)))
    cap = prod[prod.status != 2].pivot_table(index="seq", columns="stream", values="t_capture")
    if {0, 1} <= set(cap.columns):
        line("|skew stream1 - stream0|", np.abs((cap[1] - cap[0]).dropna() / 1e3))
    ok = prod[prod.status == 0]
    line("capture -> enqueue", (ok.t_enqueue - ok.t_capture) / 1e3)

    if (d / "consumer.csv").exists():
        c = pd.read_csv(d / "consumer.csv")
        p = c[(c.status == 0) & (c.stream == 0)]
        print(f"consumer ({len(c)} rows): processed={int((c.status == 0).sum())} skipped={int((c.status == 1).sum())}")
        line("transport (enqueue -> dequeue)", (p.t_dequeue - p.t_enqueue) / 1e3)
        if (p.t_infer_end > 0).any():
            line("preprocess", (p.t_infer_start - p.t_dequeue) / 1e3)
            line("inference", (p.t_infer_end - p.t_infer_start) / 1e3)
            line("END-TO-END (capture -> infer end)", (p.t_infer_end - p.t_capture) / 1e3)
        else:
            line("END-TO-END (capture -> dequeue)", (p.t_dequeue - p.t_capture) / 1e3)
        print(f"  max ring occupancy seen: {int(c.occupancy.max())}")


if __name__ == "__main__":
    main()
