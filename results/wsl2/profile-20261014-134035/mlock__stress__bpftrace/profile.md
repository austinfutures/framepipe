# Scheduler profile — mlock / stress  |  env: wsl2, kernel 6.18.33.2-microsoft-standard-WSL2

Window: first frame capture → last inference end (15.0 s); events outside it (start-up, warm-up) are excluded.

Tool: bpftrace runq_fp.bt. Scheduling delay = time a thread was runnable but waiting for a CPU.

| thread | role | scheduling delay |
|---|---|---|
| fp_cons | consumer + inference | n=1,197  p50 5.0  p99 64.5  max 1,532.0 µs |
| fp_prod0 | stream 0 capture | n=903  p50 16.0  p99 29.0  max 311.0 µs |
| fp_prod1 | stream 1 capture | n=901  p50 15.0  p99 43.0  max 79.0 µs |
| fp_producer | main/setup thread, not in the frame path | n=2  p50 2.0  p99 3.0  max 3.0 µs |

Slowest 1% of stream-0 frames (e2e ≥ 2,146 µs): 2 of 9 overlap a run-queue wait ≥ 200 µs of fp_prod0 or fp_cons. The other 7 are not explained by the guest scheduler.

Observer effect: 0 of 2 preemptions of our threads were by the tracer itself.

Who preempted our threads (count):

| our thread | preempted by | count |
|---|---|---:|
| fp_prod0 | migration/10 | 2 |
