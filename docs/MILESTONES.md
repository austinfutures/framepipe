# Milestones

Do these in order. After each one, run the commands, paste the output back into the chat,
and answer the interview questions out loud before moving on. Numbers only ever come from
your runs.

Run every command from the repo root (the consumer's default model path is relative).
Tag every pasted result with the environment from `results/env.json` (`wsl2`, `vm-…`, `native`).

---

## M0 — Environment

```bash
scripts/setup_deps.sh
scripts/build.sh
sudo scripts/check_env.sh
```

**What to paste:** the full `check_env.sh` output.

**Why it matters.** The same code gives very different tails on WSL2, a VM and bare metal.
`check_env.sh` records the kernel, preemption model, CPU count, clocksource and which tools work,
and every result is filed under that label.

**What each environment can do (expected, to be confirmed by your check):**

| | WSL2 | Ubuntu VM | native Ubuntu (USB) |
|---|---|---|---|
| SCHED_FIFO / mlockall | yes, inside the guest | yes, inside the guest | yes |
| perf sched | usually not packaged; build from WSL2 kernel source or skip | yes (no HW counters) | yes |
| bpftrace sched tracepoints | depends on the WSL kernel config | usually yes | yes |
| V4L2 webcam | needs usbipd + UVC kernel support; usually no | with USB passthrough | yes |
| host noise in the tail | yes | yes | no |

Fallbacks: no perf → bpftrace only. No bpftrace → perf only. Neither → the producer's own
wake-up lateness is still a scheduler-jitter measurement, it just can't attribute the cause.

**Interview questions**
1. Why can SCHED_FIFO inside a VM still show multi-millisecond stalls?
2. What's the difference between a PREEMPT_DYNAMIC kernel and PREEMPT_RT, and which would you ship on a robot?
3. Why does the clocksource (tsc vs kvm-clock vs hyperv) matter for a latency measurement?

---

## M1 — The ring buffer

```bash
build/fp_ring_test               # 20 M messages
build/fp_ring_test 100000000     # longer
stress-ng --cpu $(nproc) --timeout 60s & build/fp_ring_test; wait
```

**What to paste:** the PASS/FAIL lines.

**Design choices to understand (`src/common/spsc_ring.hpp`):**
- One writer per index → plain stores, no compare-and-swap.
- `publish()` = store-release of `head`; consumer's load-acquire of `head` makes the slot contents visible.
- `release()` = store-release of `tail`; producer's load-acquire of `tail` stops it overwriting a slot still being read.
- `head` and `tail` on separate cache lines (false sharing); each side caches the other's index.
- Free-running 64-bit counters; `& (capacity-1)` for the slot, so capacity is a power of two.

**Interview questions**
1. What breaks if `publish()` used `memory_order_relaxed`? Would you see it on x86? On ARM?
2. Why is this SPSC and not MPMC, and what would MPMC cost you?
3. Why does `std::atomic` in shared memory need to be lock-free?

---

## M2 — The frame source alone

```bash
build/fp_producer --standalone --duration 20 --out /tmp/m2
.venv/bin/python scripts/quick_stats.py /tmp/m2
sudo build/fp_producer --standalone --duration 20 --fifo 80 --out /tmp/m2fifo
.venv/bin/python scripts/quick_stats.py /tmp/m2fifo
```

**What to paste:** both `quick_stats.py` outputs (wake-up lateness, |period jitter|, skew, capture→enqueue).

**Design choices (`src/producer.cpp`):**
- Absolute deadlines `t0 + k*period` with `clock_nanosleep(TIMER_ABSTIME)`: errors never accumulate.
- If a wake-up is later than the next deadline, the skipped frames are logged as `missed_deadline`, not silently produced late.
- Both stream threads share `t0`, so any capture-time difference is pure scheduling skew.
- Default SCHED_OTHER threads have 50 µs timer slack; RT threads have zero. That alone shifts the wake-up numbers.

**Interview questions**
1. Why absolute rather than relative sleeps for a periodic loop?
2. Your two streams are scheduled at the same instant — where does the skew between them come from?
3. How would this differ with a hardware-triggered camera, and which timestamp would you trust?

---

## M3 — Transport between processes

```bash
# terminal 1
build/fp_consumer --model none --transport copy --out /tmp/m3copy
# terminal 2
build/fp_producer --transport copy --duration 20 --out /tmp/m3copy
.venv/bin/python scripts/quick_stats.py /tmp/m3copy
# repeat with --transport zerocopy on BOTH sides into /tmp/m3zc
```

**What to paste:** both outputs. Look at capture→enqueue (includes the producer's copy) and transport.

**Design choices (`src/consumer.cpp`, `src/common/shm.hpp`):**
- One POSIX shm segment, page-aligned regions, `MAP_POPULATE` so the timed loop never page-faults.
- Data path is lock-free; only the *wake-up* uses a syscall (futex). The consumer reads the doorbell before checking the rings, so no wake-up can be lost.
- Zero-copy holds the slot until preprocessing is done; copy mode frees it immediately. That's the trade-off.
- With `--model none` the consumer still reads sampled pixels, so zero-copy isn't "free" because nobody looked.

**Interview questions**
1. Why a futex instead of busy-polling or a pipe/eventfd? What does each cost?
2. What happens when the consumer is slower than 60 Hz with a ring of 8? What does `--latest-only` change, and which would a robot policy want?
3. Copying 900 KB takes how long on your machine, according to your numbers? Is that the bottleneck?

---

## M4 — Inference in the loop

```bash
build/fp_consumer --transport zerocopy --out /tmp/m4 &
build/fp_producer --transport zerocopy --duration 20 --out /tmp/m4; wait
.venv/bin/python scripts/quick_stats.py /tmp/m4
```

**What to paste:** the consumer's model/warm-up lines and the stats.

**Design choices (`src/policy_model.cpp`):**
- 1 intra-op thread: inference runs on our (pinned, prioritised) thread, no thread-pool hand-off.
- Tensors wrap buffers allocated once; warm-up runs before the producer is allowed to start.
- Denormals flushed to zero; arena allocator kept so steady-state runs don't call malloc.
- FIFO priority is applied *after* model load/warm-up so a slow load can't starve the machine.

**Interview questions**
1. Why might the first inferences be slower, and how do you keep that out of the measurement?
2. When would you use more intra-op threads, and what would it do to jitter?
3. What's your frame budget at 60 Hz, and what fraction does inference take in your run?

---

## M5 — Measurement and plots for one configuration

```bash
./run_experiments.sh --only baseline --duration 30
```

**What to paste:** the printed `results.md` and look at `results/<env>/<tag>/baseline__*/latency.png`.

**Definitions** (also in `scripts/analyze.py`): e2e = capture → inference end; wake-up lateness;
|period jitter|; skew; drops = missed deadlines + ring-full + consumer skips + lost in transit.

**Interview questions**
1. Why report p99, p99.9 and max rather than the mean?
2. How many frames do you need before p99.9 means anything? How many did you have?
3. Why is logging done into a preallocated vector instead of writing the CSV as you go?

---

## M6 — All experiments

```bash
./run_experiments.sh --duration 30 --reps 3     # ~1 h; start with --reps 1
```

**What to paste:** `results.md` (all tables, including the two contribution tables).

**How to read it.** The ladder table says what each fix added on top of the previous ones. The
ablation table says what removing one fix costs with everything else on. If they disagree, the
fixes interact (e.g. pinning matters little until the load is kept off the pinned CPUs).

**Interview questions**
1. Which single fix mattered most in *your* data, idle vs. under load, and why that one?
2. Did any fix make things worse? What's a plausible mechanism?
3. What would you need to change to claim these results hold on the robot rather than on this machine?

---

## M7 — Profiling: why the jitter happens

```bash
sudo scripts/profile.sh --configs baseline,mlock --duration 15
```

**What to paste:** each `profile.md` (scheduling-delay table, slow-frame attribution, preemptor table).

**What the tools show.** `perf sched timehist` gives, for every time one of our threads got the CPU,
how long it had been runnable but waiting (sched delay). `scripts/runq_fp.bt` measures the same
from tracepoints, records who preempted us, and stamps events with `CLOCK_MONOTONIC` so
`profile_report.py` can line them up with individual slow frames.

**Interview questions**
1. What exactly is run-queue latency, and how is it different from the time spent in `clock_nanosleep`?
2. Under SCHED_FIFO, what can still delay your thread? (IRQs, softirqs, higher-priority RT threads, RT throttling, the hypervisor…)
3. What's the overhead of tracing every `sched_switch` system-wide, and how did you keep it from biasing the measurement?

---

## M8 — Stretch (only after M0–M7)

- **GStreamer H.264 over RTP/UDP on localhost**, measuring capture-to-display latency.
- **IRQ affinity**: move IRQs off the pipeline CPUs (`/proc/irq/*/smp_affinity_list`) and re-run `isolate`.
- **Timer slack**: `--timer-slack 1` on both binaries without FIFO, to separate "RT priority" from "zero slack".

Ask for these when you get here; they're not in the repo yet.

---

## M9 — Write-up

1. Paste `results.md` and the `profile.md` files into the chat.
2. We fill in the README summary, results and "why" sections from those numbers only.
3. We write the project-bank Typst entry (`docs/project_bank_entry.typ`) from the same numbers.
