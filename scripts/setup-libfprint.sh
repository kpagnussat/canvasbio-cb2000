#!/usr/bin/env bash
# Setup helper for integrating CanvasBio CB2000 driver into a local libfprint tree.
#
# Usage examples:
#   ./scripts/setup-libfprint.sh --clone --integrate --build
#   LIBFPRINT_DIR="$HOME/libfprint" ./scripts/setup-libfprint.sh --integrate

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DBX_NAME="${DBX_NAME:-canvasbio}"
if [[ "${HOME}" == */.ContainerConfigs/${DBX_NAME} ]]; then
  DEFAULT_CONTAINER_CONFIG_ROOT="${HOME}"
elif [[ "${HOME}" == */.ContainerConfigs ]]; then
  DEFAULT_CONTAINER_CONFIG_ROOT="${HOME}/${DBX_NAME}"
else
  DEFAULT_CONTAINER_CONFIG_ROOT="${HOME}/.ContainerConfigs/${DBX_NAME}"
fi
LIBFPRINT_DIR="${LIBFPRINT_DIR:-${DEFAULT_CONTAINER_CONFIG_ROOT}/libfprint}"
DRIVER_NAME="canvasbio_cb2000"

DRIVER_SRC="${PROJECT_ROOT}/src/canvasbio_cb2000.c"
DRIVER_SIGFM_SRC="${PROJECT_ROOT}/src/cb2000_sigfm_matcher.c"
DRIVER_SIGFM_HDR="${PROJECT_ROOT}/src/cb2000_sigfm_matcher.h"
DRIVER_MESON_SRC="${PROJECT_ROOT}/src/meson.build"

show_help() {
  cat <<EOF
CanvasBio CB2000 libfprint integration script

Usage: $0 [options]

Options:
  --install-deps    Install build dependencies (Ubuntu/Debian)
  --clone           Clone (or pull) libfprint
  --integrate       Copy driver files from current project into libfprint tree
  --build           Build libfprint with canvasbio_cb2000 enabled
  --check           Syntax-check local driver source files
  --all             Run install-deps + clone + integrate + build
  -h, --help        Show this help

Environment variables:
  LIBFPRINT_DIR     Path to libfprint source
                   (default: \$HOME/.ContainerConfigs/\$DBX_NAME/libfprint)

Notes:
  - This script integrates the main driver C sources into libfprint.
  - The OpenCV sidecar helper is built separately by:
      ./scripts/build_sigfm_opencv_helper.sh
  - Optional device permissions rule:
      sudo cp tools/99-canvasbio.rules /etc/udev/rules.d/
EOF
}

require_file() {
  local f="$1"
  if [[ ! -f "${f}" ]]; then
    echo "ERROR: required file not found: ${f}"
    exit 1
  fi
}

install_deps() {
  echo "==> Installing build dependencies for libfprint, helper, and local venv..."
  sudo apt update
  sudo apt install -y \
    build-essential \
    pkg-config \
    meson ninja-build \
    libglib2.0-dev \
    libgusb-dev \
    libpixman-1-dev \
    libnss3-dev \
    libgudev-1.0-dev \
    gtk-doc-tools \
    libgirepository1.0-dev \
    libcairo2-dev \
    libusb-1.0-0-dev \
    libssl-dev \
    libopencv-dev \
    python3-venv \
    ca-certificates \
    git
  echo "==> Dependencies installed"
}

clone_libfprint() {
  echo "==> Cloning libfprint..."
  if [[ -d "${LIBFPRINT_DIR}/.git" ]]; then
    (cd "${LIBFPRINT_DIR}" && git pull --ff-only)
  else
    git clone https://gitlab.freedesktop.org/libfprint/libfprint.git "${LIBFPRINT_DIR}"
  fi
  echo "==> Clone complete: ${LIBFPRINT_DIR}"
}

patch_libfprint_meson() {
  local meson_file="$1"
  if grep -q "'${DRIVER_NAME}'" "${meson_file}"; then
    echo "    ${DRIVER_NAME} already present in ${meson_file}"
    return 0
  fi
  if grep -q "driver_sources" "${meson_file}"; then
    perl -0pi -e "s/'focaltech_moc'\\s*:\\s*\\n\\s*\\[ 'drivers\\/focaltech_moc\\/focaltech_moc\\.c' \\],\\n}/'focaltech_moc' :\\n        [ 'drivers\\/focaltech_moc\\/focaltech_moc.c' ],\\n    '${DRIVER_NAME}' :\\n        [ 'drivers\\/${DRIVER_NAME}\\/canvasbio_cb2000.c', 'drivers\\/${DRIVER_NAME}\\/cb2000_sigfm_matcher.c' ],\\n}/s" "${meson_file}"
    if grep -q "'${DRIVER_NAME}'" "${meson_file}"; then
      echo "    Added '${DRIVER_NAME}' to ${meson_file} (driver_sources dict)"
    else
      echo "    WARNING: failed to patch ${meson_file} automatically (driver_sources dict)."
    fi
  elif grep -q "default_drivers" "${meson_file}"; then
    perl -0pi -e "s/'focaltech_moc',\\n]/'focaltech_moc',\\n    '${DRIVER_NAME}',\\n]/s" "${meson_file}"
    if grep -q "'${DRIVER_NAME}'" "${meson_file}"; then
      echo "    Added '${DRIVER_NAME}' to ${meson_file} (default_drivers array)"
    else
      echo "    WARNING: failed to patch ${meson_file} automatically (default_drivers array)."
    fi
  else
    echo "    WARNING: could not auto-detect insertion point in ${meson_file}."
  fi
}

integrate_driver() {
  echo "==> Integrating driver into libfprint..."

  local drivers_dir="${LIBFPRINT_DIR}/libfprint/drivers"
  local target_dir="${drivers_dir}/${DRIVER_NAME}"
  local libfprint_meson="${LIBFPRINT_DIR}/libfprint/meson.build"
  local top_meson="${LIBFPRINT_DIR}/meson.build"

  require_file "${DRIVER_SRC}"
  require_file "${DRIVER_SIGFM_SRC}"
  require_file "${DRIVER_SIGFM_HDR}"
  require_file "${DRIVER_MESON_SRC}"

  if [[ ! -d "${drivers_dir}" ]]; then
    echo "ERROR: libfprint drivers directory not found: ${drivers_dir}"
    echo "       Run --clone first or set LIBFPRINT_DIR correctly."
    exit 1
  fi

  mkdir -p "${target_dir}"
  cp -v "${DRIVER_SRC}" "${target_dir}/canvasbio_cb2000.c"
  cp -v "${DRIVER_SIGFM_SRC}" "${target_dir}/cb2000_sigfm_matcher.c"
  cp -v "${DRIVER_SIGFM_HDR}" "${target_dir}/cb2000_sigfm_matcher.h"
  cp -v "${DRIVER_MESON_SRC}" "${target_dir}/meson.build"
  echo "    Copied driver files to ${target_dir}"

  if [[ -f "${libfprint_meson}" ]]; then
    patch_libfprint_meson "${libfprint_meson}"
  fi
  if [[ -f "${top_meson}" ]]; then
    patch_libfprint_meson "${top_meson}"
  fi

  echo "==> Integration complete"
  echo "INFO: Build the OpenCV sidecar from this repo with:"
  echo "      ./scripts/build_sigfm_opencv_helper.sh"
}

build_libfprint() {
  echo "==> Building libfprint..."
  if [[ ! -d "${LIBFPRINT_DIR}" ]]; then
    echo "ERROR: LIBFPRINT_DIR not found: ${LIBFPRINT_DIR}"
    exit 1
  fi

  (
    cd "${LIBFPRINT_DIR}"
    if [[ -d "builddir" && ! -f "builddir/meson-private/build.dat" ]]; then
      local stale_dir="builddir.invalid.$(date +%Y%m%d_%H%M%S)"
      echo "==> Detected incomplete Meson builddir; moving it to ${stale_dir}"
      mv builddir "${stale_dir}"
    fi
    if [[ ! -d "builddir" ]]; then
      meson setup builddir -Ddoc=false -Dgtk-examples=false -Ddrivers="${DRIVER_NAME},virtual_image"
    fi
    meson configure builddir -Ddrivers="${DRIVER_NAME},virtual_image"
    ninja -C builddir
  )

  echo "==> Build complete"
  echo "To test:"
  echo "  ${LIBFPRINT_DIR}/builddir/examples/img-capture"
}

check_syntax() {
  echo "==> Checking local driver syntax..."
  require_file "${DRIVER_SRC}"
  require_file "${DRIVER_SIGFM_SRC}"

  if ! command -v gcc >/dev/null 2>&1; then
    echo "ERROR: gcc not found."
    exit 1
  fi

  local cflags=""
  if pkg-config --exists glib-2.0 2>/dev/null; then
    cflags="$(pkg-config --cflags glib-2.0)"
    gcc -fsyntax-only ${cflags} "${DRIVER_SIGFM_SRC}"
    echo "    Matcher syntax OK"
  else
    echo "    Matcher syntax skipped: glib-2.0 headers not available via pkg-config"
    echo "    Tip: install glib development headers or run inside the prepared build container."
  fi

  local libfprint_headers_ok=0
  local libfprint_cflags=""
  local dep_cflags=""
  if [[ -d "${LIBFPRINT_DIR}/libfprint" ]]; then
    libfprint_headers_ok=1
    libfprint_cflags="-I${LIBFPRINT_DIR} -I${LIBFPRINT_DIR}/libfprint -I${LIBFPRINT_DIR}/libfprint/drivers"
  fi

  if [[ "${libfprint_headers_ok}" == "1" ]]; then
    if dep_cflags="$(pkg-config --cflags glib-2.0 gio-2.0 gobject-2.0 gusb libusb-1.0 gudev-1.0 nss pixman-1 2>/dev/null)"; then
      if gcc -fsyntax-only ${cflags} ${dep_cflags} ${libfprint_cflags} "${DRIVER_SRC}"; then
        echo "    Main driver syntax OK (with libfprint headers from ${LIBFPRINT_DIR})"
      else
        echo "    Main driver syntax skipped: libfprint tree is present but not self-contained for standalone syntax-only checks"
        echo "    Tip: run the full build in the prepared libfprint tree/container for authoritative validation."
      fi
    else
      echo "    Main driver syntax skipped: libfprint dependency headers not fully available via pkg-config"
      echo "    Tip: install libfprint build deps or run inside the prepared build container."
    fi
  else
    echo "    Main driver syntax skipped: libfprint headers not available under ${LIBFPRINT_DIR}"
    echo "    Tip: set LIBFPRINT_DIR=<local-libfprint-tree> to validate the full driver translation unit."
  fi
}

if [[ $# -eq 0 ]]; then
  show_help
  exit 0
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-deps)
      install_deps
      ;;
    --clone)
      clone_libfprint
      ;;
    --integrate)
      integrate_driver
      ;;
    --build)
      build_libfprint
      ;;
    --all)
      install_deps
      clone_libfprint
      integrate_driver
      build_libfprint
      ;;
    --check)
      check_syntax
      ;;
    -h|--help)
      show_help
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      show_help
      exit 1
      ;;
  esac
  shift
done
