# Scheduler profile — baseline / stress  |  env: wsl2, kernel 6.18.33.2-microsoft-standard-WSL2

Window: first frame capture → last inference end (15.0 s); events outside it (start-up, warm-up) are excluded.

Tool: bpftrace runq_fp.bt. Scheduling delay = time a thread was runnable but waiting for a CPU.

| thread | role | scheduling delay |
|---|---|---|
| fp_cons | consumer + inference | n=2,456  p50 39.0  p99 5,835.5  max 14,193.0 µs |
| fp_prod0 | stream 0 capture | n=7,933  p50 39.0  p99 4,045.7  max 14,993.0 µs |
| fp_prod1 | stream 1 capture | n=3,781  p50 26.0  p99 4,168.6  max 10,720.0 µs |

Slowest 1% of stream-0 frames (e2e ≥ 15,005 µs): 9 of 9 overlap a run-queue wait ≥ 200 µs of fp_prod0 or fp_cons. The other 0 are not explained by the guest scheduler.

Observer effect: 10897 of 11050 preemptions of our threads were by the tracer itself.

Who preempted our threads (count):

| our thread | preempted by | count |
|---|---|---:|
| fp_prod0 | bpftrace | 7012 |
| fp_prod1 | bpftrace | 2786 |
| fp_cons | bpftrace | 1099 |
| fp_prod1 | stress-ng | 40 |
| fp_cons | stress-ng | 26 |
| fp_prod1 | kworker/0:1H | 11 |
| fp_prod1 | kworker/2:1H | 11 |
| fp_prod1 | kworker/u48:1 | 9 |
| fp_prod1 | kworker/u48:3 | 9 |
| fp_prod1 | kworker/u48:0 | 8 |
