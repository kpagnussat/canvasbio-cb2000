#!/usr/bin/env bash
set -euo pipefail

# Collects latest CB2000 runtime artifacts into a canonical folder.
# Works even when logs/images are scattered across multiple paths.

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
RUNTIME_ROOT="${CB2000_RUNTIME_ROOT:-${CONTAINER_CONFIG_ROOT}/cb2000_runtime}"
RUN_ID="${1:-$(date +%Y%m%d_%H%M%S)}"
RUN_DIR="${RUNTIME_ROOT}/runs/${RUN_ID}"
LATEST_DIR="${RUNTIME_ROOT}/latest"

mkdir -p "${RUN_DIR}" "${LATEST_DIR}"

CANDIDATE_DIRS=(
  "${HOME}"
  "${CONTAINER_CONFIG_ROOT}"
  "${LIBFPRINT_ROOT}"
  "${LIBFPRINT_ROOT}/builddir/examples"
  "${LIBFPRINT_ROOT}/builddir/examples/canvasbio_raw"
  "${RUNTIME_ROOT}/latest"
  "${RUNTIME_ROOT}/runs"
)

pick_latest() {
  local pattern="$1"
  local found
  found="$(find "${CANDIDATE_DIRS[@]}" -maxdepth 2 -type f \
      ! -path "${RUN_DIR}/*" ! -path "${LATEST_DIR}/*" \
      -name "${pattern}" -printf '%T@ %p\n' 2>/dev/null \
      | sort -nr | sed -n '1p' | cut -d' ' -f2- || true)"
  if [[ -n "${found}" ]]; then
    printf '%s' "${found}"
  fi
}

copy_latest_as() {
  local pattern="$1"
  local dest_name="$2"
  local src
  src="$(pick_latest "${pattern}")"
  if [[ -n "${src}" ]]; then
    cp -f "${src}" "${RUN_DIR}/${dest_name}"
    cp -f "${src}" "${LATEST_DIR}/${dest_name}"
    printf '%s|%s\n' "${dest_name}" "${src}" >> "${RUN_DIR}/manifest.txt"
  fi
}

printf 'run_id=%s\n' "${RUN_ID}" > "${RUN_DIR}/manifest.txt"
printf 'collected_at=%s\n' "$(date --iso-8601=seconds)" >> "${RUN_DIR}/manifest.txt"
printf 'runtime_root=%s\n' "${RUNTIME_ROOT}" >> "${RUN_DIR}/manifest.txt"
printf '\n' >> "${RUN_DIR}/manifest.txt"

# Canonical outputs
copy_latest_as "capture_log*.txt" "capture_log.txt"
copy_latest_as "enroll_log*.txt" "enroll_log.txt"
copy_latest_as "verify_log*.txt" "verify_log.txt"
copy_latest_as "finger.pgm" "finger.pgm"
copy_latest_as "enrolled.pgm" "enrolled.pgm"
copy_latest_as "verify.pgm" "verify.pgm"
copy_latest_as "test-storage.variant" "test-storage.variant"
copy_latest_as "canvasbio_*.raw" "canvasbio_latest.raw"

# Keep all verify logs from known locations for deep debug
mkdir -p "${RUN_DIR}/verify_logs_all" "${LATEST_DIR}/verify_logs_all"
find "${CANDIDATE_DIRS[@]}" -maxdepth 2 -type f \
  ! -path "${RUN_DIR}/*" ! -path "${LATEST_DIR}/*" \
  -name "verify_log*.txt" 2>/dev/null | while read -r f; do
  base="$(basename "${f}")"
  cp -f "${f}" "${RUN_DIR}/verify_logs_all/${base}"
  cp -f "${f}" "${LATEST_DIR}/verify_logs_all/${base}"
done || true

# Also expose verify logs sorted by mtime (most recent first), useful when
# there are verify_log.txt + verify_log2/3/4 in the same round.
rank=1
find "${CANDIDATE_DIRS[@]}" -maxdepth 2 -type f \
  ! -path "${RUN_DIR}/*" ! -path "${LATEST_DIR}/*" \
  -name "verify_log*.txt" -printf '%T@ %p\n' 2>/dev/null \
  | sort -nr | while read -r line; do
      src="${line#* }"
      out="$(printf 'verify_log_%02d_latest.txt' "${rank}")"
      cp -f "${src}" "${RUN_DIR}/${out}"
      cp -f "${src}" "${LATEST_DIR}/${out}"
      printf '%s|%s\n' "${out}" "${src}" >> "${RUN_DIR}/manifest.txt"
      rank=$((rank + 1))
    done || true

echo "Collected runtime artifacts:"
echo "  run   : ${RUN_DIR}"
echo "  latest: ${LATEST_DIR}"
echo "  manifest: ${RUN_DIR}/manifest.txt"
