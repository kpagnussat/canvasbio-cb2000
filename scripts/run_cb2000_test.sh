#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_DRIVER_SRC="${PROJECT_ROOT}/src/canvasbio_cb2000.c"
DEFAULT_DRIVER_SIGFM_SRC="${PROJECT_ROOT}/src/cb2000_sigfm_matcher.c"
DEFAULT_DRIVER_SIGFM_HDR="${PROJECT_ROOT}/src/cb2000_sigfm_matcher.h"
DRIVER_SRC="${CB2000_DRIVER_SRC:-${DEFAULT_DRIVER_SRC}}"
DRIVER_SIGFM_SRC="${CB2000_DRIVER_SIGFM_SRC:-${DEFAULT_DRIVER_SIGFM_SRC}}"
DRIVER_SIGFM_HDR="${CB2000_DRIVER_SIGFM_HDR:-${DEFAULT_DRIVER_SIGFM_HDR}}"
SIGFM_OPENCV_HELPER_BUILD_SCRIPT="${CB2000_SIGFM_HELPER_BUILD_SCRIPT:-${PROJECT_ROOT}/scripts/build_sigfm_opencv_helper.sh}"
SIGFM_OPENCV_HELPER_SO="${CB2000_SIGFM_HELPER_SO:-${PROJECT_ROOT}/build/libcb2000_sigfm_opencv.so}"
USB_ACCESS_HELPER="${PROJECT_ROOT}/scripts/cb2000_usb_host_access.sh"
DBX_NAME="${DBX_NAME:-canvasbio}"
if [[ "${HOME}" == */.ContainerConfigs/${DBX_NAME} ]]; then
  DEFAULT_CONTAINER_CONFIG_ROOT="${HOME}"
elif [[ "${HOME}" == */.ContainerConfigs ]]; then
  DEFAULT_CONTAINER_CONFIG_ROOT="${HOME}/${DBX_NAME}"
else
  DEFAULT_CONTAINER_CONFIG_ROOT="${HOME}/.ContainerConfigs/${DBX_NAME}"
fi
CONTAINER_CONFIG_ROOT="${CB2000_CONTAINER_CONFIG_ROOT:-${DEFAULT_CONTAINER_CONFIG_ROOT}}"
LIBFPRINT_ROOT="${CB2000_LIBFPRINT_ROOT:-${CONTAINER_CONFIG_ROOT}/libfprint}"
LIBFPRINT_DRIVER_DIR="${CB2000_LIBFPRINT_DRIVER_DIR:-${LIBFPRINT_ROOT}/libfprint/drivers/canvasbio_cb2000}"
BUILD_DIR="${CB2000_LIBFPRINT_BUILD_DIR:-${LIBFPRINT_ROOT}/builddir}"
EXAMPLES_DIR="${CB2000_EXAMPLES_DIR:-${BUILD_DIR}/examples}"

TS="$(date +%Y%m%d_%H%M%S)"
_LOG_ROOT="${CB2000_LOG_ROOT:-${PROJECT_ROOT}/../dev_logs/sessions}"
LOG_DIR="${_LOG_ROOT}/${TS}"
RUNTIME_ROOT="${CB2000_RUNTIME_ROOT:-${CONTAINER_CONFIG_ROOT}/cb2000_runtime}"
RUNTIME_RUN_DIR="${RUNTIME_ROOT}/runs/${TS}"
RUNTIME_LATEST_DIR="${RUNTIME_ROOT}/latest"
USE_SUDO="${CB2000_USE_SUDO:-0}"
AUTO_FIX_USB="${CB2000_AUTO_FIX_USB:-0}"
mkdir -p "${LOG_DIR}"
mkdir -p "${RUNTIME_RUN_DIR}" "${RUNTIME_LATEST_DIR}"

if [[ "${1:-}" != "--in-container" && "${1:-}" != "--no-container" ]]; then
  echo "== Ensuring container '${DBX_NAME}' is active =="
  if command -v distrobox >/dev/null 2>&1; then
    # Re-exec inside container via distrobox.
    DBX_CMD="${DBX_CMD:-distrobox enter}"
    exec ${DBX_CMD} "${DBX_NAME}" -- /bin/bash -lc "\"$0\" --in-container"
  fi

  if command -v podman >/dev/null 2>&1; then
    # Fallback: enter container via podman exec.
    CANDIDATE="$(podman ps --format '{{.Names}}' | grep -E "^${DBX_NAME}$|^distrobox-${DBX_NAME}$" | head -n1 || true)"
    if [[ -z "${CANDIDATE}" ]]; then
      cat <<EOF
ERROR: podman found, but container '${DBX_NAME}' not running.
Running containers:
$(podman ps --format '{{.Names}}')
EOF
      exit 1
    fi
    exec podman exec -it "${CANDIDATE}" /bin/bash -lc "\"$0\" --in-container"
  fi

  cat <<EOF
ERROR: neither 'distrobox' nor 'podman' found in PATH.
If you are already inside the container, re-run with: $0 --in-container
EOF
  exit 1
fi

echo "== CB2000 test run (${TS}) =="
echo "Logs: ${LOG_DIR}"
echo "Runtime artifacts: ${RUNTIME_RUN_DIR}"

if [[ "${USE_SUDO}" == "1" ]]; then
  RUNNER=(sudo -E)
else
  RUNNER=()
fi

copy_if_exists() {
  local src="$1"
  local dst="$2"
  if [[ -f "${src}" ]]; then
    cp -f "${src}" "${dst}"
  fi
}

ensure_usb_access() {
  local rc=0

  if [[ ! -f "${USB_ACCESS_HELPER}" ]]; then
    return 0
  fi

  set +e
  bash "${USB_ACCESS_HELPER}" --check --quiet
  rc=$?
  set -e

  if [[ "${rc}" == "11" && "${AUTO_FIX_USB}" == "1" ]]; then
    bash "${USB_ACCESS_HELPER}" --ensure
    set +e
    bash "${USB_ACCESS_HELPER}" --check --quiet
    rc=$?
    set -e
  fi

  case "${rc}" in
    0)
      return 0
      ;;
    10)
      cat <<EOF
ERROR: nenhum CanvasBio CB2000 foi detectado.
Conecte o leitor e tente novamente.
EOF
      exit 1
      ;;
    11)
      cat <<EOF
ERROR: o CB2000 foi detectado, mas o usuario atual nao tem permissao de escrita no USB.
Execute antes:
  bash "${USB_ACCESS_HELPER}" --ensure

Se quiser que este runner tente corrigir automaticamente:
  CB2000_AUTO_FIX_USB=1 bash "$0" --in-container
EOF
      exit 1
      ;;
    *)
      echo "WARN: falha inesperada na verificacao de acesso USB (rc=${rc})."
      ;;
  esac
}

# Driver-side RAW/PGM outputs are forced into a canonical folder.
export CB2000_OUTPUT_DIR="${RUNTIME_RUN_DIR}"
export LD_LIBRARY_PATH="${PROJECT_ROOT}/build:${LD_LIBRARY_PATH:-}"

if [[ ! -f "${DRIVER_SRC}" ]]; then
  echo "ERROR: Driver source not found: ${DRIVER_SRC}"
  exit 1
fi

if [[ ! -f "${DRIVER_SIGFM_SRC}" ]]; then
  echo "ERROR: SIGFM matcher source not found: ${DRIVER_SIGFM_SRC}"
  exit 1
fi

if [[ ! -f "${DRIVER_SIGFM_HDR}" ]]; then
  echo "ERROR: SIGFM matcher header not found: ${DRIVER_SIGFM_HDR}"
  exit 1
fi

if [[ ! -d "${LIBFPRINT_DRIVER_DIR}" ]]; then
  echo "ERROR: libfprint driver dir not found: ${LIBFPRINT_DRIVER_DIR}"
  exit 1
fi

if [[ ! -f "${SIGFM_OPENCV_HELPER_BUILD_SCRIPT}" ]]; then
  echo "ERROR: SIGFM OpenCV helper build script not found: ${SIGFM_OPENCV_HELPER_BUILD_SCRIPT}"
  exit 1
fi

echo "== Building SIGFM OpenCV helper (mandatory) =="
bash "${SIGFM_OPENCV_HELPER_BUILD_SCRIPT}" 2>&1 | tee "${LOG_DIR}/build_sigfm_opencv_helper.log"

if [[ ! -f "${SIGFM_OPENCV_HELPER_SO}" ]]; then
  echo "ERROR: SIGFM OpenCV helper shared library not found after build: ${SIGFM_OPENCV_HELPER_SO}"
  exit 1
fi

echo "== Driver sources selected =="
echo "driver      : ${DRIVER_SRC}"
echo "sigfm src   : ${DRIVER_SIGFM_SRC}"
echo "sigfm header: ${DRIVER_SIGFM_HDR}"

echo "== Copying driver =="
cp -v "${DRIVER_SRC}" "${LIBFPRINT_DRIVER_DIR}/canvasbio_cb2000.c" | tee "${LOG_DIR}/copy_driver.log"
cp -v "${DRIVER_SIGFM_SRC}" "${LIBFPRINT_DRIVER_DIR}/cb2000_sigfm_matcher.c" | tee -a "${LOG_DIR}/copy_driver.log"
cp -v "${DRIVER_SIGFM_HDR}" "${LIBFPRINT_DRIVER_DIR}/cb2000_sigfm_matcher.h" | tee -a "${LOG_DIR}/copy_driver.log"

echo "== Building libfprint =="
(cd "${LIBFPRINT_ROOT}" && ninja -C builddir) 2>&1 | tee "${LOG_DIR}/build.log"

echo "== USB access preflight =="
ensure_usb_access

echo "== img-capture =="
echo "Place finger when prompted. Press Ctrl+C to abort." | tee "${LOG_DIR}/img-capture.log" "${RUNTIME_RUN_DIR}/capture_log.txt"
set +e
(cd "${RUNTIME_RUN_DIR}" && G_MESSAGES_DEBUG=all "${RUNNER[@]}" "${EXAMPLES_DIR}/img-capture") \
  2>&1 | tee -a "${LOG_DIR}/img-capture.log" | tee -a "${RUNTIME_RUN_DIR}/capture_log.txt"
IMG_CAPTURE_RC=${PIPESTATUS[0]}
set -e
echo "img-capture exit code: ${IMG_CAPTURE_RC}" | tee -a "${LOG_DIR}/img-capture.log" | tee -a "${RUNTIME_RUN_DIR}/capture_log.txt"

echo "== enroll =="
echo "Follow prompts for 15 lift-and-shift scans. Press Ctrl+C to abort." | tee "${LOG_DIR}/enroll.log" "${RUNTIME_RUN_DIR}/enroll_log.txt"
set +e
(cd "${RUNTIME_RUN_DIR}" && G_MESSAGES_DEBUG=all "${RUNNER[@]}" "${EXAMPLES_DIR}/enroll") \
  2>&1 | tee -a "${LOG_DIR}/enroll.log" | tee -a "${RUNTIME_RUN_DIR}/enroll_log.txt"
ENROLL_RC=${PIPESTATUS[0]}
set -e
echo "enroll exit code: ${ENROLL_RC}" | tee -a "${LOG_DIR}/enroll.log" | tee -a "${RUNTIME_RUN_DIR}/enroll_log.txt"

echo "== verify =="
echo "Follow prompts. Press Ctrl+C to abort." | tee "${LOG_DIR}/verify.log" "${RUNTIME_RUN_DIR}/verify_log.txt"
set +e
(cd "${RUNTIME_RUN_DIR}" && G_MESSAGES_DEBUG=all "${RUNNER[@]}" "${EXAMPLES_DIR}/verify") \
  2>&1 | tee -a "${LOG_DIR}/verify.log" | tee -a "${RUNTIME_RUN_DIR}/verify_log.txt"
VERIFY_RC=${PIPESTATUS[0]}
set -e
echo "verify exit code: ${VERIFY_RC}" | tee -a "${LOG_DIR}/verify.log" | tee -a "${RUNTIME_RUN_DIR}/verify_log.txt"

# Keep a stable "latest" view for docs/analysis tooling.
copy_if_exists "${RUNTIME_RUN_DIR}/capture_log.txt" "${RUNTIME_LATEST_DIR}/capture_log.txt"
copy_if_exists "${RUNTIME_RUN_DIR}/enroll_log.txt" "${RUNTIME_LATEST_DIR}/enroll_log.txt"
copy_if_exists "${RUNTIME_RUN_DIR}/verify_log.txt" "${RUNTIME_LATEST_DIR}/verify_log.txt"
copy_if_exists "${RUNTIME_RUN_DIR}/finger.pgm" "${RUNTIME_LATEST_DIR}/finger.pgm"
copy_if_exists "${RUNTIME_RUN_DIR}/enrolled.pgm" "${RUNTIME_LATEST_DIR}/enrolled.pgm"
copy_if_exists "${RUNTIME_RUN_DIR}/verify.pgm" "${RUNTIME_LATEST_DIR}/verify.pgm"
copy_if_exists "${RUNTIME_RUN_DIR}/test-storage.variant" "${RUNTIME_LATEST_DIR}/test-storage.variant"
find "${RUNTIME_RUN_DIR}" -maxdepth 1 -type f -name "canvasbio_*.raw" -exec cp -f {} "${RUNTIME_LATEST_DIR}/" \; || true

echo "== Done =="
echo "Logs saved to: ${LOG_DIR}"
echo "Runtime run saved to: ${RUNTIME_RUN_DIR}"
echo "Runtime latest: ${RUNTIME_LATEST_DIR}"
