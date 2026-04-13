#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC_FILE="${PROJECT_ROOT}/src/cb2000_sigfm_opencv_helper.cpp"
OUT_DIR="${PROJECT_ROOT}/build"
OUT_FILE="${OUT_DIR}/libcb2000_sigfm_opencv.so"
PKG="${CB2000_OPENCV_PKG:-}"

if [[ ! -f "${SRC_FILE}" ]]; then
  echo "ERROR: helper source not found: ${SRC_FILE}"
  exit 1
fi

if ! command -v g++ >/dev/null 2>&1; then
  echo "ERROR: g++ not found."
  exit 1
fi

if ! command -v pkg-config >/dev/null 2>&1; then
  echo "ERROR: pkg-config not found."
  exit 1
fi

if [[ -z "${PKG}" ]]; then
  if pkg-config --exists opencv4 2>/dev/null; then
    PKG="opencv4"
  elif pkg-config --exists opencv 2>/dev/null; then
    PKG="opencv"
  else
    PKG="$(pkg-config --list-all | awk '/opencv/ {print $1; exit}')"
  fi
fi

if [[ -z "${PKG}" ]] || ! pkg-config --exists "${PKG}" 2>/dev/null; then
  echo "ERROR: OpenCV pkg-config entry not available (selected='${PKG:-<none>}')."
  echo "INFO: available pkg-config entries matching 'opencv':"
  pkg-config --list-all | awk '/opencv/ {print "  - " $1}' || true
  echo "INFO: if needed, set explicitly: CB2000_OPENCV_PKG=<pkg-name>"
  exit 1
fi

mkdir -p "${OUT_DIR}"

echo "== Building SIGFM OpenCV helper (${PKG}) =="
g++ -std=c++17 -O2 -fPIC -shared \
  "${SRC_FILE}" \
  -o "${OUT_FILE}" \
  $(pkg-config --cflags --libs "${PKG}")

echo "Built: ${OUT_FILE}"
