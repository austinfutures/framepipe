#!/usr/bin/env bash
# setup_deps.sh — install everything framepipe needs (all free), on Ubuntu 22.04/24.04.
#   * apt: compiler, CMake, stress-ng, bpftrace, v4l-utils, perf (where packaged)
#   * ONNX Runtime prebuilt CPU release -> third_party/onnxruntime
#   * Python venv with numpy/pandas/matplotlib/onnx -> .venv
#   * the tiny ONNX policy model -> models/tiny_policy.onnx
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORT_VERSION="${ORT_VERSION:-1.20.1}"

echo "== apt packages"
sudo apt-get update
sudo apt-get install -y build-essential cmake git curl python3 python3-venv python3-pip \
  stress-ng bpftrace v4l-utils util-linux

# perf: Ubuntu ships it per kernel version. On WSL2 the Microsoft kernel has no
# matching package, so we only try, and explain the fallback if it fails.
if ! sudo apt-get install -y linux-tools-common "linux-tools-$(uname -r)" 2>/dev/null; then
  echo
  echo "!! perf for kernel $(uname -r) is not packaged (expected on WSL2)."
  echo "   Fallback: build perf from the matching kernel source, e.g. on WSL2:"
  echo "     sudo apt-get install -y flex bison libelf-dev libdw-dev libtraceevent-dev libslang2-dev"
  echo "     git clone --depth 1 https://github.com/microsoft/WSL2-Linux-Kernel"
  echo "     make -C WSL2-Linux-Kernel/tools/perf && sudo cp WSL2-Linux-Kernel/tools/perf/perf /usr/local/bin/"
  echo "   (or skip perf and use bpftrace only — scripts/check_env.sh reports what works)"
  echo
fi

echo "== ONNX Runtime ${ORT_VERSION}"
case "$(uname -m)" in
  x86_64) ORT_PKG="onnxruntime-linux-x64-${ORT_VERSION}" ;;
  aarch64) ORT_PKG="onnxruntime-linux-aarch64-${ORT_VERSION}" ;;
  *) echo "unsupported arch $(uname -m)"; exit 1 ;;
esac
mkdir -p "$REPO/third_party"
if [ ! -f "$REPO/third_party/onnxruntime/include/onnxruntime_cxx_api.h" ]; then
  curl -fL -o "$REPO/third_party/${ORT_PKG}.tgz" \
    "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_PKG}.tgz"
  tar -xzf "$REPO/third_party/${ORT_PKG}.tgz" -C "$REPO/third_party"
  rm -rf "$REPO/third_party/onnxruntime"
  mv "$REPO/third_party/${ORT_PKG}" "$REPO/third_party/onnxruntime"
  rm -f "$REPO/third_party/${ORT_PKG}.tgz"
fi

echo "== Python venv"
python3 -m venv "$REPO/.venv"
"$REPO/.venv/bin/pip" install --upgrade pip
"$REPO/.venv/bin/pip" install numpy pandas matplotlib onnx

echo "== model"
"$REPO/.venv/bin/python" "$REPO/scripts/make_model.py" --out "$REPO/models/tiny_policy.onnx"

echo "== done. Next: scripts/build.sh && sudo scripts/check_env.sh"
