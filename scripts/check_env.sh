#!/usr/bin/env bash
# check_env.sh — what does THIS machine support? Run as root:  sudo scripts/check_env.sh
#
# Prints a report and writes results/env.json. The "label" (wsl2 / vm-<type> / native)
# is used by run_experiments.sh to file every result under results/<label>/, so no
# number is ever separated from the environment it was measured in.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$REPO/results"

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root (sudo) so the scheduler/perf/bpftrace checks are meaningful"; exit 1
fi

ok()   { printf '  [ OK ] %s\n' "$*"; }
no()   { printf '  [ NO ] %s\n' "$*"; }
info() { printf '  [info] %s\n' "$*"; }

# ---------------------------------------------------------------- environment label
if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
  ENV_KIND=wsl2
else
  VIRT="$(systemd-detect-virt 2>/dev/null || true)"
  if [ -z "$VIRT" ] || [ "$VIRT" = "none" ]; then ENV_KIND=native; else ENV_KIND="vm-$VIRT"; fi
fi
LABEL="${ENV_LABEL:-$ENV_KIND}"

KERNEL="$(uname -r)"
KVER="$(uname -v)"
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
NPROC="$(nproc)"
CMDLINE="$(cat /proc/cmdline)"
ISOLATED="$(cat /sys/devices/system/cpu/isolated 2>/dev/null || true)"
CLOCKSRC="$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || echo unknown)"
GOVERNOR="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
RT_RUNTIME="$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || echo unknown)"
PREEMPT="unknown"
case "$KVER" in *PREEMPT_RT*) PREEMPT="PREEMPT_RT" ;; *PREEMPT_DYNAMIC*) PREEMPT="PREEMPT_DYNAMIC" ;; *PREEMPT*) PREEMPT="PREEMPT" ;; esac
if [ -r /sys/kernel/debug/sched/preempt ]; then PREEMPT="$PREEMPT ($(cat /sys/kernel/debug/sched/preempt))"; fi

echo "== environment"
info "label: $LABEL   (override with ENV_LABEL=...)"
info "kernel: $KERNEL   preemption: $PREEMPT"
info "CPU: $CPU_MODEL   x $NPROC logical CPUs"
info "clocksource: $CLOCKSRC   governor: $GOVERNOR   sched_rt_runtime_us: $RT_RUNTIME"
[ -n "$ISOLATED" ] && ok "isolated CPUs (isolcpus): $ISOLATED" || info "no isolcpus on the kernel cmdline (isolation will be emulated by keeping load off the pinned CPUs)"
[ "$NPROC" -ge 4 ] && ok "$NPROC CPUs: enough to pin producer x2 + consumer on separate cores" \
                   || no "only $NPROC CPUs: pinning will share cores (give the VM >= 4 vCPUs if you can)"

echo "== real-time knobs"
FIFO=no; MLOCK=no
if chrt -f 50 true 2>/dev/null; then ok "SCHED_FIFO (chrt -f 50)"; FIFO=yes; else no "SCHED_FIFO refused"; fi
if [ -x "$REPO/build/fp_producer" ]; then
  if "$REPO/build/fp_producer" --standalone --streams 1 --duration 0.2 --fifo 50 --mlock --out /tmp >/dev/null 2>&1; then
    ok "fp_producer with --fifo --mlock runs"; MLOCK=yes
  else
    no "fp_producer --fifo --mlock failed: $("$REPO/build/fp_producer" --standalone --streams 1 --duration 0.2 --fifo 50 --mlock --out /tmp 2>&1 | tail -1)"
  fi
  rm -f /tmp/producer.csv
else
  info "build/fp_producer not built yet; build then re-run to test mlockall"
fi
case "$LABEL" in wsl2*|vm-*)
  info "virtualised: the guest scheduler is real, but the host can deschedule the whole vCPU."
  info "SCHED_FIFO controls ordering INSIDE the guest only; host-induced stalls stay visible." ;;
esac

echo "== tracing"
PERF=no; BPFTRACE=no; BTF=no
if command -v perf >/dev/null 2>&1 && perf sched record -o /tmp/fp_perf_check.data -- sleep 0.2 >/dev/null 2>&1; then
  ok "perf sched record ($(perf --version 2>/dev/null))"; PERF=yes
else
  no "perf sched not usable (WSL2: build perf from WSL2-Linux-Kernel/tools/perf; see setup_deps.sh)"
fi
rm -f /tmp/fp_perf_check.data*
[ -e /sys/kernel/btf/vmlinux ] && { ok "kernel BTF present"; BTF=yes; } || info "no /sys/kernel/btf/vmlinux (tracepoint scripts still work)"
if command -v bpftrace >/dev/null 2>&1 && timeout 20 bpftrace -e 'tracepoint:sched:sched_switch { exit(); }' >/dev/null 2>&1; then
  ok "bpftrace tracepoints ($(bpftrace --version 2>/dev/null))"; BPFTRACE=yes
else
  no "bpftrace cannot attach to sched tracepoints here"
fi

echo "== camera"
V4L2=no
if ls /dev/video* >/dev/null 2>&1; then
  ok "V4L2 devices: $(ls /dev/video* | tr '\n' ' ')"; V4L2=yes
  command -v v4l2-ctl >/dev/null && v4l2-ctl --list-devices 2>/dev/null | sed 's/^/         /'
else
  no "no /dev/video* (synthetic frames only; WSL2 needs usbipd-win + a UVC-enabled kernel)"
fi

ENV_KIND="$ENV_KIND" LABEL="$LABEL" KERNEL="$KERNEL" KVER="$KVER" CPU_MODEL="$CPU_MODEL" NPROC="$NPROC" \
CMDLINE="$CMDLINE" ISOLATED="$ISOLATED" CLOCKSRC="$CLOCKSRC" GOVERNOR="$GOVERNOR" RT_RUNTIME="$RT_RUNTIME" \
PREEMPT="$PREEMPT" FIFO="$FIFO" MLOCK="$MLOCK" PERF="$PERF" BPFTRACE="$BPFTRACE" BTF="$BTF" V4L2="$V4L2" \
python3 - "$REPO/results/env.json" <<'PY'
import json, os, sys
keys = ["ENV_KIND","LABEL","KERNEL","KVER","CPU_MODEL","NPROC","CMDLINE","ISOLATED","CLOCKSRC","GOVERNOR",
        "RT_RUNTIME","PREEMPT","FIFO","MLOCK","PERF","BPFTRACE","BTF","V4L2"]
d = {k.lower(): os.environ.get(k, "") for k in keys}
json.dump(d, open(sys.argv[1], "w"), indent=2)
print(f"\nwrote {sys.argv[1]}")
PY
