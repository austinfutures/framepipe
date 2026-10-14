framepipe: (
  title: "framepipe: Real-Time Camera-to-Policy Pipeline",
  tech: "C++17, Linux, POSIX shared memory, futex, ONNX Runtime, SCHED_FIFO, bpftrace, Python",
  link: "https://github.com/austinfutures/framepipe",
  demo: none,
  bullets: (
    ("Tail latency", "Cut p99 camera-to-policy latency 4.9x under stress-ng on WSL2, 8,396 to 1,727 µs, with SCHED_FIFO, CPU isolation and zero-copy shared memory."),
    ("Jitter", "Reduced p99 frame-period jitter 6.1x, 6,416 to 1,047 µs, and two-camera capture skew 6.8x, 6,748 to 989 µs, with zero dropped frames."),
    ("Profiling", "Built bpftrace run-queue tracing on CLOCK_MONOTONIC; all 9 slowest baseline frames overlapped CPU waits, while tuned capture-thread p99 wait was 29 µs."),
    ("Ablation", "Ran 60 experiments with ablations: removing CPU isolation added 1,249 µs p99, while SCHED_FIFO added only 1 µs once CPUs were isolated."),
  ),
),
