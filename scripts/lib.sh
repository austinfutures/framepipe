#!/usr/bin/env bash
# lib.sh — shared helpers for run_experiments.sh and profile.sh (sourced, not run).

FP_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRODUCER="$FP_REPO/build/fp_producer"
CONSUMER="$FP_REPO/build/fp_consumer"
MODEL="${MODEL:-$FP_REPO/models/tiny_policy.onnx}"
SHM_NAME="/framepipe"
FPS=60            # integer; the analysis reads it from meta.json
PROD_PRIO=80      # producer threads above the consumer: never let inference delay a capture
CONS_PRIO=70
STRESS_ARGS="${STRESS_ARGS:---vm 2 --vm-bytes 256M --io 1}"   # plus --cpu <nproc>, see start_stress
PY="$FP_REPO/.venv/bin/python"; [ -x "$PY" ] || PY=python3

# ------------------------------------------------------------------ experiment table
# name -> transport fifo pin isolate mlock
#   cumulative ladder: baseline -> zerocopy -> fifo -> pin -> isolate -> mlock (= everything on)
#   ablations ("all minus X"): everything on except X — X's individual contribution
config_fields() {
  case "$1" in
    baseline)         echo "copy     0 0 0 0" ;;
    zerocopy)         echo "zerocopy 0 0 0 0" ;;
    fifo)             echo "zerocopy 1 0 0 0" ;;
    pin)              echo "zerocopy 1 1 0 0" ;;
    isolate)          echo "zerocopy 1 1 1 0" ;;
    mlock)            echo "zerocopy 1 1 1 1" ;;
    no_zerocopy)      echo "copy     1 1 1 1" ;;
    no_fifo)          echo "zerocopy 0 1 1 1" ;;
    no_isolate)       echo "zerocopy 1 1 0 1" ;;
    no_pin_isolate)   echo "zerocopy 1 0 0 1" ;;
    # "no_mlock" is identical to "isolate", so the analysis reuses that run.
    *) return 1 ;;
  esac
}
LADDER="baseline zerocopy fifo pin isolate mlock"
ABLATIONS="no_zerocopy no_fifo no_isolate no_pin_isolate"

# ------------------------------------------------------------------ CPU plan
# Highest-numbered CPUs go to the pipeline; CPU 0.. stay "housekeeping" (where the
# kernel, IRQs by default, and — when isolating — the load generator run).
plan_cpus() {
  local n; n="$(nproc)"
  if [ "$n" -ge 4 ]; then
    CONS_CPU=$((n - 1)); PROD_CPUS="$((n - 2)),$((n - 3))"; HK_CPUS="0-$((n - 4))"
  elif [ "$n" -eq 3 ]; then
    CONS_CPU=2; PROD_CPUS="1,1"; HK_CPUS="0"
  elif [ "$n" -eq 2 ]; then
    CONS_CPU=1; PROD_CPUS="0,0"; HK_CPUS=""
  else
    CONS_CPU=""; PROD_CPUS=""; HK_CPUS=""
  fi
}

# ------------------------------------------------------------------ load generator
# start_stress ISOLATE SECONDS   -> sets STRESS_PID, STRESS_DESC
start_stress() {
  local isolate=$1 secs=$2
  # shellcheck disable=SC2206
  local cmd=(stress-ng --cpu "$(nproc)" $STRESS_ARGS --timeout "${secs}s" --quiet)
  if [ "$isolate" = 1 ] && [ -n "$HK_CPUS" ]; then
    taskset -c "$HK_CPUS" "${cmd[@]}" >/dev/null 2>&1 &
    STRESS_DESC="taskset -c $HK_CPUS ${cmd[*]}"
  else
    "${cmd[@]}" >/dev/null 2>&1 &
    STRESS_DESC="${cmd[*]}"
  fi
  STRESS_PID=$!
  sleep 2  # let the load ramp up before frames start
}

stop_stress() {
  if [ -n "${STRESS_PID:-}" ]; then
    kill -TERM "$STRESS_PID" 2>/dev/null || true
    wait "$STRESS_PID" 2>/dev/null || true
    STRESS_PID=""
  fi
}

# ------------------------------------------------------------------ one pipeline run
# run_pipeline OUTDIR TRANSPORT FIFO PIN MLOCK DURATION   (returns non-zero on failure)
run_pipeline() {
  local out=$1 transport=$2 fifo=$3 pin=$4 mlock=$5 dur=$6
  local maxf=$(( (dur + 10) * FPS * 2 + 1000 ))
  local pargs=(--transport "$transport" --duration "$dur" --fps "$FPS" --streams 2 --shm "$SHM_NAME" --out "$out")
  local cargs=(--transport "$transport" --model "$MODEL" --shm "$SHM_NAME" --out "$out" --max-frames "$maxf")
  if [ "$fifo" = 1 ];  then pargs+=(--fifo "$PROD_PRIO"); cargs+=(--fifo "$CONS_PRIO"); fi
  if [ "$pin" = 1 ];   then
    if [ -z "$CONS_CPU" ]; then echo "  !! not enough CPUs to pin" >&2; return 1; fi
    pargs+=(--cpus "$PROD_CPUS"); cargs+=(--cpu "$CONS_CPU")
  fi
  if [ "$mlock" = 1 ]; then pargs+=(--mlock); cargs+=(--mlock); fi

  rm -f "/dev/shm${SHM_NAME}"
  echo "$PRODUCER ${pargs[*]}" > "$out/cmd.txt"
  echo "$CONSUMER ${cargs[*]}" >> "$out/cmd.txt"
  "$PRODUCER" "${pargs[@]}" > "$out/producer.log" 2>&1 &
  local ppid=$!
  "$CONSUMER" "${cargs[@]}" > "$out/consumer.log" 2>&1 &
  local cpid=$!
  local rc=0
  wait "$ppid" || rc=$?
  wait "$cpid" || rc=$?
  return "$rc"
}

# ------------------------------------------------------------------ metadata
# write_meta OUTDIR CONFIG LOAD REP TRANSPORT FIFO PIN ISOLATE MLOCK DURATION
write_meta() {
  local out=$1
  META_CONFIG=$2 META_LOAD=$3 META_REP=$4 META_TRANSPORT=$5 META_FIFO=$6 META_PIN=$7 META_ISOLATE=$8 \
  META_MLOCK=$9 META_DURATION=${10} META_FPS=$FPS META_MODEL="$MODEL" META_STRESS="${STRESS_DESC:-none}" \
  META_PROD_CPUS="$PROD_CPUS" META_CONS_CPU="$CONS_CPU" META_HK_CPUS="$HK_CPUS" \
  META_PROD_PRIO=$PROD_PRIO META_CONS_PRIO=$CONS_PRIO \
  META_GIT="$(git -C "$FP_REPO" rev-parse --short HEAD 2>/dev/null || echo none)" \
  "$PY" - "$out/meta.json" "$FP_REPO/results/env.json" <<'PY'
import json, os, sys, datetime
m = {k[5:].lower(): v for k, v in os.environ.items() if k.startswith("META_")}
for k in ("fifo", "pin", "isolate", "mlock", "rep", "duration", "fps", "prod_prio", "cons_prio"):
    m[k] = int(m[k])
m["model"] = os.path.basename(m["model"])
m["timestamp"] = datetime.datetime.now().isoformat(timespec="seconds")
try:
    m["env"] = json.load(open(sys.argv[2]))
except OSError:
    m["env"] = {}
json.dump(m, open(sys.argv[1], "w"), indent=2)
PY
}

env_label() {
  "$PY" -c 'import json,sys; print(json.load(open(sys.argv[1]))["label"])' "$FP_REPO/results/env.json"
}

need_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "re-running with sudo (SCHED_FIFO, mlockall, perf and bpftrace need root)"
    exec sudo "$0" "$@"
  fi
}
