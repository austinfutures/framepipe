#!/usr/bin/env bash
# profile.sh — show WHY frames are late: trace the scheduler while the pipeline runs.
#
#   scripts/profile.sh                                  # baseline vs mlock (= all fixes), under stress, both tools
#   scripts/profile.sh --configs baseline,fifo --tool bpftrace --duration 20
#
# Each tool gets its own pipeline run (so one tracer's overhead never pollutes the
# other's numbers). Output: results/<env>/profile-<tag>/<config>__<load>__<tool>/
#   perf:     timehist_fp.txt (every switch-in of an fp_ thread: wait, sched delay, run time),
#             latency.txt (perf sched latency --sort max), the pipeline CSVs
#   bpftrace: runq.txt (CSV events + histograms), the pipeline CSVs
# then scripts/profile_report.py turns each directory into plots + a short report.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
need_root "$@"

CONFIGS="baseline mlock"; LOAD=stress; DURATION=15; TOOL=both; TAG="$(date +%Y%m%d-%H%M%S)"
while [ $# -gt 0 ]; do
  case "$1" in
    --configs) CONFIGS="${2//,/ }"; shift 2 ;;
    --load) LOAD=$2; shift 2 ;;
    --duration) DURATION=$2; shift 2 ;;
    --tool) TOOL=$2; shift 2 ;;
    --tag) TAG=$2; shift 2 ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done

[ -x "$PRODUCER" ] || { echo "build first"; exit 1; }
[ -f "$FP_REPO/results/env.json" ] || "$FP_REPO/scripts/check_env.sh"
LABEL="$(env_label)"
plan_cpus
OUT="$FP_REPO/results/$LABEL/profile-$TAG"
mkdir -p "$OUT"

TOOLS=""
case "$TOOL" in perf|bpftrace) TOOLS=$TOOL ;; both) TOOLS="perf bpftrace" ;; *) echo "--tool perf|bpftrace|both"; exit 2 ;; esac
HAVE_PERF=0; command -v perf >/dev/null 2>&1 && HAVE_PERF=1
HAVE_BPF=0;  command -v bpftrace >/dev/null 2>&1 && HAVE_BPF=1

trap 'stop_stress; rm -f /dev/shm${SHM_NAME}' EXIT
DIRS=()
for cfg in $CONFIGS; do
  read -r transport fifo pin isolate mlock <<<"$(config_fields "$cfg")"
  for tool in $TOOLS; do
    if [ "$tool" = perf ] && [ $HAVE_PERF = 0 ]; then echo "skip perf: not installed (see check_env.sh)"; continue; fi
    if [ "$tool" = bpftrace ] && [ $HAVE_BPF = 0 ]; then echo "skip bpftrace: not installed"; continue; fi
    d="$OUT/${cfg}__${LOAD}__${tool}"; mkdir -p "$d"
    echo "== $cfg / $LOAD / $tool"
    STRESS_DESC="none"
    [ "$LOAD" = stress ] && start_stress "$isolate" $((DURATION + 40))
    write_meta "$d" "$cfg" "$LOAD" 1 "$transport" "$fifo" "$pin" "$isolate" "$mlock" "$DURATION"

    if [ "$tool" = perf ]; then
      # -a: all CPUs, so we also see what ran instead of us.
      perf sched record -a -o "$d/perf.sched.data" -- sleep $((DURATION + 5)) > "$d/perf.log" 2>&1 &
      TPID=$!; sleep 1
    else
      # Keep the tracer's userspace reader on housekeeping CPUs so it competes with the
      # load, not with the pipeline (kernel-side probes still fire on every CPU).
      PIN=(); [ -n "$HK_CPUS" ] && PIN=(taskset -c "$HK_CPUS")
      "${PIN[@]}" timeout -s INT $((DURATION + 8)) bpftrace "$FP_REPO/scripts/runq_fp.bt" > "$d/runq.txt" 2> "$d/bpftrace.err" &
      TPID=$!; sleep 4   # bpftrace needs a moment to compile and attach
    fi

    run_pipeline "$d" "$transport" "$fifo" "$pin" "$mlock" "$DURATION" || echo "  !! pipeline failed (see $d/*.log)"
    wait "$TPID" || true
    stop_stress

    if [ "$tool" = perf ]; then
      perf sched timehist -i "$d/perf.sched.data" > "$d/timehist.txt" 2> "$d/timehist.err" || true
      grep -E ' fp_' "$d/timehist.txt" > "$d/timehist_fp.txt" || true
      perf sched latency -i "$d/perf.sched.data" --sort max > "$d/latency.txt" 2>/dev/null || true
      rm -f "$d/timehist.txt"   # full system-wide timehist is huge; the fp_ subset is what we use
    fi
    DIRS+=("$d")
    sleep 3
  done
done
trap - EXIT
rm -f "/dev/shm${SHM_NAME}"

if [ -n "${SUDO_USER:-}" ]; then chown -R "$SUDO_USER":"$(id -gn "$SUDO_USER")" "$FP_REPO/results"; fi
for d in "${DIRS[@]}"; do
  if [ -n "${SUDO_USER:-}" ]; then sudo -u "$SUDO_USER" "$PY" "$FP_REPO/scripts/profile_report.py" "$d" || true
  else "$PY" "$FP_REPO/scripts/profile_report.py" "$d" || true; fi
done
echo "== done: $OUT"
