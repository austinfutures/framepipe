#!/usr/bin/env bash
# run_experiments.sh — reproduce every experiment, then analyse and plot.
#
#   ./run_experiments.sh                      # full set: ladder + ablations, idle + stress, 30 s each
#   ./run_experiments.sh --duration 60 --reps 3
#   ./run_experiments.sh --only baseline,fifo --loads stress
#   ./run_experiments.sh --no-ablation
#
# Results go to results/<env-label>/<tag>/<config>__<load>__r<rep>/ with producer.csv,
# consumer.csv, logs, cmd.txt and meta.json; the analysis writes summary.csv,
# results.md and plots into results/<env-label>/<tag>/.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/scripts/lib.sh"
need_root "$@"

DURATION=30; REPS=1; ABLATION=1; LOADS="idle stress"; ONLY=""; TAG="$(date +%Y%m%d-%H%M%S)"
while [ $# -gt 0 ]; do
  case "$1" in
    --duration) DURATION=$2; shift 2 ;;
    --reps) REPS=$2; shift 2 ;;
    --no-ablation) ABLATION=0; shift ;;
    --loads) LOADS=$2; shift 2 ;;
    --only) ONLY=$2; shift 2 ;;
    --tag) TAG=$2; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done

# ---- preflight
[ -x "$PRODUCER" ] && [ -x "$CONSUMER" ] || { echo "build first: scripts/build.sh"; exit 1; }
[ -f "$MODEL" ] || { echo "model missing: run scripts/setup_deps.sh (or scripts/make_model.py)"; exit 1; }
command -v stress-ng >/dev/null || { echo "stress-ng missing: sudo apt install stress-ng"; exit 1; }
[ -f "$FP_REPO/results/env.json" ] || "$FP_REPO/scripts/check_env.sh"
LABEL="$(env_label)"
plan_cpus
OUT="$FP_REPO/results/$LABEL/$TAG"
mkdir -p "$OUT"

if [ -n "$ONLY" ]; then CONFIGS="${ONLY//,/ }"
else CONFIGS="$LADDER"; [ "$ABLATION" = 1 ] && CONFIGS="$CONFIGS $ABLATIONS"; fi
for c in $CONFIGS; do config_fields "$c" >/dev/null || { echo "unknown config $c"; exit 2; }; done

n_runs=$(( $(echo $CONFIGS | wc -w) * $(echo $LOADS | wc -w) * REPS ))
echo "== env=$LABEL  out=$OUT"
echo "== CPUs: producer threads -> $PROD_CPUS, consumer -> $CONS_CPU, housekeeping -> ${HK_CPUS:-none}"
echo "== $n_runs runs x ${DURATION}s  (~$(( n_runs * (DURATION + 8) / 60 )) min)"

trap 'stop_stress; rm -f /dev/shm${SHM_NAME}' EXIT
FAILED=()
i=0
for rep in $(seq 1 "$REPS"); do
  for cfg in $CONFIGS; do
    read -r transport fifo pin isolate mlock <<<"$(config_fields "$cfg")"
    for load in $LOADS; do
      i=$((i + 1))
      d="$OUT/${cfg}__${load}__r${rep}"
      mkdir -p "$d"
      printf '[%d/%d] %-15s %-6s rep %d ... ' "$i" "$n_runs" "$cfg" "$load" "$rep"
      STRESS_DESC="none"
      [ "$load" = stress ] && start_stress "$isolate" $((DURATION + 30))
      write_meta "$d" "$cfg" "$load" "$rep" "$transport" "$fifo" "$pin" "$isolate" "$mlock" "$DURATION"
      if run_pipeline "$d" "$transport" "$fifo" "$pin" "$mlock" "$DURATION"; then
        echo "ok   $(grep -h 'ring_full' "$d/producer.log" | sed 's/\[producer\] //' | tr '\n' ' ')"
      else
        echo "FAILED (see $d/*.log)"; FAILED+=("$d")
      fi
      stop_stress
      sleep 3  # cool-down between runs
    done
  done
done
trap - EXIT
rm -f "/dev/shm${SHM_NAME}"

# hand results back to the invoking user, then analyse as that user
if [ -n "${SUDO_USER:-}" ]; then chown -R "$SUDO_USER":"$(id -gn "$SUDO_USER")" "$FP_REPO/results"; fi
echo "== analysing"
if [ -n "${SUDO_USER:-}" ]; then sudo -u "$SUDO_USER" "$PY" "$FP_REPO/scripts/analyze.py" "$OUT"
else "$PY" "$FP_REPO/scripts/analyze.py" "$OUT"; fi

if [ ${#FAILED[@]} -gt 0 ]; then
  echo "!! ${#FAILED[@]} run(s) failed:"; printf '   %s\n' "${FAILED[@]}"; exit 1
fi
echo "== done: $OUT/results.md"
