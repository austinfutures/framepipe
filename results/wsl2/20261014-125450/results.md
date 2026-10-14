# Results — environment `wsl2`

Kernel `6.18.33.2-microsoft-standard-WSL2` (PREEMPT_DYNAMIC ((none) voluntary full lazy )), 12 CPUs (13th Gen Intel(R) Core(TM) i7-1355U), clocksource `tsc`. 30 s per run at 60 Hz, model `tiny_policy.onnx`, 3 rep(s) per cell (median across reps). The exact stress-ng command is in each `*__stress__*/meta.json`; the exact pipeline commands are in `cmd.txt`.

All latencies in µs. e2e = capture → inference end (stream 0). wake = producer wake-up lateness. |jitter| = |frame period − 16,667 µs|. skew = |capture time difference| between the two streams.

## idle

| config | e2e p50 | e2e p99 | e2e p99.9 | e2e max | wake p99 | wake max | jitter p99 (abs) | drops | skew p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline (default sched, copy) | 2,447 | 3,602 | 4,855 | 5,027 | 2,006 | 6,963 | 1,128 | 0 | 882 |
| + zero-copy | 2,128 | 3,159 | 3,683 | 4,672 | 1,931 | 7,654 | 1,098 | 0 | 887 |
| + SCHED_FIFO | 1,817 | 2,942 | 3,604 | 4,120 | 1,823 | 4,537 | 1,121 | 0 | 1,013 |
| + CPU pinning | 1,906 | 2,992 | 3,735 | 3,931 | 1,882 | 8,171 | 1,112 | 0 | 966 |
| + isolation | 1,767 | 2,920 | 3,581 | 5,581 | 1,894 | 7,097 | 1,094 | 0 | 947 |
| + mlockall (all on) | 2,154 | 3,053 | 3,724 | 6,494 | 1,811 | 7,256 | 1,130 | 0 | 921 |
| all on − zero-copy | 2,363 | 3,444 | 3,808 | 3,949 | 1,901 | 6,456 | 1,165 | 0 | 967 |
| all on − SCHED_FIFO | 2,066 | 2,940 | 3,473 | 6,797 | 1,848 | 8,846 | 1,187 | 0 | 962 |
| all on − isolation | 2,127 | 3,073 | 4,351 | 16,382 | 1,834 | 8,669 | 1,154 | 0 | 955 |
| all on − pinning + isolation | 2,111 | 3,031 | 3,720 | 5,104 | 1,954 | 7,181 | 1,058 | 0 | 887 |

## stress

| config | e2e p50 | e2e p99 | e2e p99.9 | e2e max | wake p99 | wake max | jitter p99 (abs) | drops | skew p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline (default sched, copy) | 1,678 | 8,396 | 14,094 | 16,064 | 5,438 | 10,980 | 6,416 | 0 | 6,748 |
| + zero-copy | 1,326 | 8,901 | 24,237 | 25,222 | 5,130 | 21,232 | 6,216 | 1 | 5,584 |
| + SCHED_FIFO | 1,227 | 2,729 | 4,599 | 5,729 | 917 | 4,637 | 1,234 | 0 | 1,067 |
| + CPU pinning | 982 | 2,572 | 4,838 | 9,327 | 835 | 6,069 | 1,129 | 0 | 1,471 |
| + isolation | 981 | 1,681 | 2,266 | 3,431 | 1,391 | 6,714 | 970 | 0 | 922 |
| + mlockall (all on) | 1,012 | 1,727 | 2,200 | 2,719 | 1,382 | 6,880 | 1,047 | 0 | 989 |
| all on − zero-copy | 1,236 | 2,127 | 2,847 | 3,623 | 1,385 | 7,152 | 977 | 0 | 918 |
| all on − SCHED_FIFO | 1,164 | 1,729 | 2,720 | 3,253 | 1,497 | 6,024 | 961 | 0 | 928 |
| all on − isolation | 1,055 | 2,977 | 8,464 | 13,165 | 841 | 12,692 | 1,349 | 0 | 1,281 |
| all on − pinning + isolation | 1,221 | 2,720 | 5,503 | 10,609 | 862 | 14,851 | 1,759 | 0 | 1,704 |

## Contribution of each fix, cumulative (idle)

Change vs. the previous row of the ladder (negative = better).

| step | Δ e2e p99 | Δ e2e max | Δ wake p99 | Δ wake max |
|---|---:|---:|---:|---:|
| + zero-copy | -444 | -355 | -76 | +691 |
| + SCHED_FIFO | -217 | -551 | -108 | -3,117 |
| + CPU pinning | +50 | -189 | +59 | +3,634 |
| + isolation | -72 | +1,650 | +12 | -1,074 |
| + mlockall (all on) | +134 | +912 | -83 | +159 |

## Contribution of each fix, ablation (idle)

Cost of removing one fix from the all-on configuration (positive = that fix was helping).

| removed | Δ e2e p99 | Δ e2e max | Δ wake p99 | Δ wake max |
|---|---:|---:|---:|---:|
| zero-copy | +390 | -2,545 | +90 | -800 |
| SCHED_FIFO | -114 | +303 | +37 | +1,591 |
| isolation | +19 | +9,889 | +23 | +1,414 |
| pinning + isolation | -23 | -1,389 | +143 | -75 |
| mlockall | -134 | -912 | +83 | -159 |

## Contribution of each fix, cumulative (stress)

Change vs. the previous row of the ladder (negative = better).

| step | Δ e2e p99 | Δ e2e max | Δ wake p99 | Δ wake max |
|---|---:|---:|---:|---:|
| + zero-copy | +506 | +9,158 | -308 | +10,253 |
| + SCHED_FIFO | -6,172 | -19,493 | -4,214 | -16,595 |
| + CPU pinning | -157 | +3,598 | -82 | +1,432 |
| + isolation | -891 | -5,897 | +556 | +645 |
| + mlockall (all on) | +46 | -712 | -9 | +166 |

## Contribution of each fix, ablation (stress)

Cost of removing one fix from the all-on configuration (positive = that fix was helping).

| removed | Δ e2e p99 | Δ e2e max | Δ wake p99 | Δ wake max |
|---|---:|---:|---:|---:|
| zero-copy | +400 | +904 | +3 | +272 |
| SCHED_FIFO | +1 | +534 | +114 | -856 |
| isolation | +1,249 | +10,445 | -541 | +5,812 |
| pinning + isolation | +993 | +7,890 | -520 | +7,971 |
| mlockall | -46 | +712 | +9 | -166 |
