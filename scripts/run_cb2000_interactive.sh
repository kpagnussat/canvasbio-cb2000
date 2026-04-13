#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVER_SRC="${PROJECT_ROOT}/src/canvasbio_cb2000.c"
DRIVER_SIGFM_SRC="${PROJECT_ROOT}/src/cb2000_sigfm_matcher.c"
DRIVER_SIGFM_HDR="${PROJECT_ROOT}/src/cb2000_sigfm_matcher.h"
SIGFM_OPENCV_HELPER_BUILD_SCRIPT="${CB2000_SIGFM_HELPER_BUILD_SCRIPT:-${PROJECT_ROOT}/scripts/packaging/build_sigfm_opencv_helper.sh}"
SIGFM_OPENCV_HELPER_SO="${CB2000_SIGFM_HELPER_SO:-${PROJECT_ROOT}/build/libcb2000_sigfm_opencv.so}"
USB_ACCESS_HELPER="${PROJECT_ROOT}/scripts/setup/cb2000_usb_host_access.sh"
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
MESON_SETUP_ARGS_DEFAULT="-Ddoc=false -Dgtk-examples=false"
ANALYZE_SCRIPT="${PROJECT_ROOT}/scripts/metrics/analyze.sh"

SESSION_ID="${CB2000_SESSION_ID:-$(date +%Y%m%d_%H%M%S)}"
RUNTIME_ROOT="${CB2000_RUNTIME_ROOT:-${CONTAINER_CONFIG_ROOT}/cb2000_runtime}"
RUN_DIR="${RUNTIME_ROOT}/runs/${SESSION_ID}"
LATEST_DIR="${RUNTIME_ROOT}/latest"
_LOG_ROOT="${CB2000_LOG_ROOT:-${PROJECT_ROOT}/../dev_logs/sessions}"
LOG_DIR="${_LOG_ROOT}/${SESSION_ID}"
USE_SUDO="${CB2000_USE_SUDO:-0}"
AUTO_FIX_USB="${CB2000_AUTO_FIX_USB:-0}"

if [[ -n "${CB2000_DRIVER_SRC:-}" || -n "${CB2000_DRIVER_SIGFM_SRC:-}" || -n "${CB2000_DRIVER_SIGFM_HDR:-}" ]]; then
  echo "INFO: overriding driver source via env is disabled in interactive mode."
  echo "INFO: always using files from ${PROJECT_ROOT}/src/"
fi

mkdir -p "${RUN_DIR}" "${LATEST_DIR}" "${LOG_DIR}" "${RUN_DIR}/attempts"

if [[ "${1:-}" != "--in-container" && "${1:-}" != "--no-container" ]]; then
  # Called from inside container context:
  # - if already inside the target container, run directly;
  # - if inside another container, bounce to host to avoid nested podman issues.
  if [[ -n "${CONTAINER_ID:-}" && "${CONTAINER_ID}" == "${DBX_NAME}" ]]; then
    exec "$0" --in-container
  fi

  if [[ -n "${CONTAINER_ID:-}" && "${CONTAINER_ID}" != "${DBX_NAME}" ]]; then
    if command -v distrobox-host-exec >/dev/null 2>&1; then
      echo "== Detected container '${CONTAINER_ID}', delegating launcher to host context =="
      exec distrobox-host-exec "$0"
    fi
  fi

  echo "== Ensuring container '${DBX_NAME}' is active =="
  if command -v distrobox >/dev/null 2>&1; then
    DBX_CMD="${DBX_CMD:-distrobox enter}"
    exec ${DBX_CMD} "${DBX_NAME}" -- /bin/bash -lc "\"$0\" --in-container"
  fi

  if command -v podman >/dev/null 2>&1; then
    CANDIDATE="$(podman ps --format '{{.Names}}' | grep -E "^${DBX_NAME}$|^distrobox-${DBX_NAME}$" | head -n1 || true)"
    if [[ -z "${CANDIDATE}" ]]; then
      cat <<MSG
ERROR: podman found, but container '${DBX_NAME}' is not running.
Running containers:
$(podman ps --format '{{.Names}}')
MSG
      exit 1
    fi
    exec podman exec -it "${CANDIDATE}" /bin/bash -lc "\"$0\" --in-container"
  fi

  cat <<MSG
ERROR: neither 'distrobox' nor 'podman' found in PATH.
If already inside the container, run: $0 --in-container
MSG
  exit 1
fi

if [[ "${USE_SUDO}" == "1" ]]; then
  RUNNER=(sudo -E)
else
  RUNNER=()
fi

export CB2000_OUTPUT_DIR="${RUN_DIR}"
export LD_LIBRARY_PATH="${PROJECT_ROOT}/build:${LD_LIBRARY_PATH:-}"

copy_runtime_latest() {
  cp -f "${RUN_DIR}/capture_log.txt" "${LATEST_DIR}/capture_log.txt" 2>/dev/null || true
  cp -f "${RUN_DIR}/enroll_log.txt" "${LATEST_DIR}/enroll_log.txt" 2>/dev/null || true
  cp -f "${RUN_DIR}/verify_log.txt" "${LATEST_DIR}/verify_log.txt" 2>/dev/null || true
  cp -f "${RUN_DIR}/finger.pgm" "${LATEST_DIR}/finger.pgm" 2>/dev/null || true
  cp -f "${RUN_DIR}/enrolled.pgm" "${LATEST_DIR}/enrolled.pgm" 2>/dev/null || true
  cp -f "${RUN_DIR}/verify.pgm" "${LATEST_DIR}/verify.pgm" 2>/dev/null || true
  cp -f "${RUN_DIR}/test-storage.variant" "${LATEST_DIR}/test-storage.variant" 2>/dev/null || true
  find "${RUN_DIR}" -maxdepth 1 -type f -name 'canvasbio_*.raw' -exec cp -f {} "${LATEST_DIR}/" \; 2>/dev/null || true
  if [[ -d "${RUN_DIR}/enroll_captures" ]]; then
    mkdir -p "${LATEST_DIR}/enroll_captures"
    find "${RUN_DIR}/enroll_captures" -maxdepth 1 -type f -name '*.pgm' -exec cp -f {} "${LATEST_DIR}/enroll_captures/" \; 2>/dev/null || true
  fi

  # Preserve all verify logs found during the session in a stable place.
  mkdir -p "${RUN_DIR}/verify_logs_all" "${LATEST_DIR}/verify_logs_all"
  find "${RUN_DIR}" -maxdepth 1 -type f -name 'verify_log*.txt' -exec cp -f {} "${RUN_DIR}/verify_logs_all/" \; 2>/dev/null || true
  find "${RUN_DIR}" -maxdepth 1 -type f -name 'verify_log*.txt' -exec cp -f {} "${LATEST_DIR}/verify_logs_all/" \; 2>/dev/null || true
}

snapshot_used_driver_files() {
  local used_dir="${RUN_DIR}/driver_used"
  local latest_used_dir="${LATEST_DIR}/driver_used"
  local src_file dst_file
  local -a manifest_files=(
    "canvasbio_cb2000.c"
    "cb2000_sigfm_matcher.c"
    "cb2000_sigfm_matcher.h"
  )
  local -a required_files=(
    "canvasbio_cb2000.c"
    "cb2000_sigfm_matcher.c"
    "cb2000_sigfm_matcher.h"
  )

  mkdir -p "${used_dir}" "${latest_used_dir}"

  for src_file in "${required_files[@]}"; do
    src_file="${LIBFPRINT_DRIVER_DIR}/${src_file}"
    if [[ ! -f "${src_file}" ]]; then
      echo "ERROR: required runtime driver file not found: ${src_file}"
      return 1
    fi
    dst_file="${used_dir}/$(basename "${src_file}")"
    cp -f "${src_file}" "${dst_file}"
    cp -f "${dst_file}" "${latest_used_dir}/"
  done

  if [[ -f "${SIGFM_OPENCV_HELPER_SO}" ]]; then
    cp -f "${SIGFM_OPENCV_HELPER_SO}" "${used_dir}/"
    cp -f "${SIGFM_OPENCV_HELPER_SO}" "${latest_used_dir}/"
    manifest_files+=("libcb2000_sigfm_opencv.so")
  else
    echo "WARN: SIGFM helper .so not found during driver snapshot: ${SIGFM_OPENCV_HELPER_SO}"
  fi

  {
    echo "session=${SESSION_ID}"
    echo "timestamp=$(date --iso-8601=seconds)"
    echo "project_driver_src=${DRIVER_SRC}"
    echo "project_sigfm_src=${DRIVER_SIGFM_SRC}"
    echo "project_sigfm_hdr=${DRIVER_SIGFM_HDR}"
    echo "runtime_driver_dir=${LIBFPRINT_DRIVER_DIR}"
    echo "helper_so=${SIGFM_OPENCV_HELPER_SO}"
  } > "${used_dir}/PROVENANCE.txt"
  cp -f "${used_dir}/PROVENANCE.txt" "${latest_used_dir}/PROVENANCE.txt"

  (
    cd "${used_dir}"
    sha256sum "${manifest_files[@]}" > MANIFEST.sha256
  )
  cp -f "${used_dir}/MANIFEST.sha256" "${latest_used_dir}/MANIFEST.sha256"

  # Also snapshot into project logs dir for analysis traceability
  local log_used_dir="${LOG_DIR}/driver_used"
  mkdir -p "${log_used_dir}"
  cp -f "${used_dir}"/*.c "${used_dir}"/*.h "${log_used_dir}/" 2>/dev/null || true
  cp -f "${used_dir}/PROVENANCE.txt" "${used_dir}/MANIFEST.sha256" "${log_used_dir}/" 2>/dev/null || true
  if [[ -f "${used_dir}/libcb2000_sigfm_opencv.so" ]]; then
    cp -f "${used_dir}/libcb2000_sigfm_opencv.so" "${log_used_dir}/"
  fi

  echo "== Runtime driver snapshot saved to: ${used_dir} ==" | tee -a "${LOG_DIR}/copy_driver.log"
  echo "== Project log driver snapshot: ${log_used_dir} ==" | tee -a "${LOG_DIR}/copy_driver.log"
}

log_session_env_usage() {
  local env_log="${LOG_DIR}/session_env.log"

  env_source() {
    local var_name="$1"
    if [[ -n "${!var_name+x}" ]]; then
      echo "env"
    else
      echo "default"
    fi
  }

  {
    echo "== Session environment usage =="
    printf "%-36s %-8s %s\n" "VARIABLE" "SOURCE" "EFFECTIVE_VALUE"
    printf "%-36s %-8s %s\n" "DBX_NAME" "$(env_source DBX_NAME)" "${DBX_NAME}"
    printf "%-36s %-8s %s\n" "CB2000_CONTAINER_CONFIG_ROOT" "$(env_source CB2000_CONTAINER_CONFIG_ROOT)" "${CONTAINER_CONFIG_ROOT}"
    printf "%-36s %-8s %s\n" "CB2000_LIBFPRINT_ROOT" "$(env_source CB2000_LIBFPRINT_ROOT)" "${LIBFPRINT_ROOT}"
    printf "%-36s %-8s %s\n" "CB2000_SESSION_ID" "$(env_source CB2000_SESSION_ID)" "${SESSION_ID}"
    printf "%-36s %-8s %s\n" "CB2000_RUNTIME_ROOT" "$(env_source CB2000_RUNTIME_ROOT)" "${RUNTIME_ROOT}"
    printf "%-36s %-8s %s\n" "CB2000_USE_SUDO" "$(env_source CB2000_USE_SUDO)" "${USE_SUDO}"
    printf "%-36s %-8s %s\n" "DBX_CMD" "$(env_source DBX_CMD)" "${DBX_CMD:-distrobox enter}"
    printf "%-36s %-8s %s\n" "CB2000_MESON_SETUP_ARGS" "$(env_source CB2000_MESON_SETUP_ARGS)" "${CB2000_MESON_SETUP_ARGS:-${MESON_SETUP_ARGS_DEFAULT}}"
    printf "%-36s %-8s %s\n" "CB2000_DRIVER_SRC" "$(env_source CB2000_DRIVER_SRC)" "${CB2000_DRIVER_SRC:-<disabled in interactive mode>}"
    printf "%-36s %-8s %s\n" "CB2000_DRIVER_SIGFM_SRC" "$(env_source CB2000_DRIVER_SIGFM_SRC)" "${CB2000_DRIVER_SIGFM_SRC:-<disabled in interactive mode>}"
    printf "%-36s %-8s %s\n" "CB2000_DRIVER_SIGFM_HDR" "$(env_source CB2000_DRIVER_SIGFM_HDR)" "${CB2000_DRIVER_SIGFM_HDR:-<disabled in interactive mode>}"
    printf "%-36s %-8s %s\n" "CB2000_OPENCV_PKG (helper passthrough)" "$(env_source CB2000_OPENCV_PKG)" "${CB2000_OPENCV_PKG:-<default from helper script>}"
    printf "%-36s %-8s %s\n" "CB2000_VERIFY_BATCH_LATEST" "$(env_source CB2000_VERIFY_BATCH_LATEST)" "${CB2000_VERIFY_BATCH_LATEST:-3}"
    printf "%-36s %-8s %s\n" "CB2000_VERIFY_BATCH_CORRECT_COUNT" "$(env_source CB2000_VERIFY_BATCH_CORRECT_COUNT)" "${CB2000_VERIFY_BATCH_CORRECT_COUNT:-<unset>}"
    printf "%-36s %-8s %s\n" "CB2000_VERIFY_BATCH_CUTOFF_TIME" "$(env_source CB2000_VERIFY_BATCH_CUTOFF_TIME)" "${CB2000_VERIFY_BATCH_CUTOFF_TIME:-<unset>}"

    echo
    echo "== Prefix scan (CB2000_/B2000_/DBX_) =="
    env | LC_ALL=C sort | grep -E '^(CB2000_|B2000_|DBX_)' || true
  } | tee "${env_log}"

  cp -f "${env_log}" "${RUN_DIR}/session_env.log"
  cp -f "${env_log}" "${LATEST_DIR}/session_env.log"
}

emit_verify_gate_summary() {
  if [[ ! -f "${ANALYZE_SCRIPT}" ]]; then
    echo "WARN: analyze script not found: ${ANALYZE_SCRIPT}"
    return 0
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    echo "WARN: python3 not found; skipping verify gate summary."
    return 0
  fi

  echo
  echo "== Verify Gate Summary =="
  "${ANALYZE_SCRIPT}" gate-summary --run "${RUN_DIR}" | tee "${LOG_DIR}/verify_gate_summary.log" || true
}

emit_verify_metrics() {
  local exclude_last="${CB2000_METRICS_EXCLUDE_LAST:-0}"

  if [[ ! -f "${ANALYZE_SCRIPT}" ]]; then
    echo "WARN: analyze script not found: ${ANALYZE_SCRIPT}"
    return 0
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    echo "WARN: python3 not found; skipping verify metrics."
    return 0
  fi

  echo
  echo "== Verify Metrics =="
  "${ANALYZE_SCRIPT}" metrics --run "${RUN_DIR}" --exclude-last "${exclude_last}" \
    | tee "${LOG_DIR}/verify_metrics.log" || true
}

emit_verify_batch_summary() {
  local latest_count="${CB2000_VERIFY_BATCH_LATEST:-3}"
  local batch_logs_root="${CB2000_VERIFY_BATCH_LOGS_ROOT:-${RUNTIME_ROOT}/runs}"
  local out_file="${RUN_DIR}/verify_batch_summary.txt"
  local -a cmd=()

  if [[ ! -f "${ANALYZE_SCRIPT}" ]]; then
    echo "WARN: analyze script not found: ${ANALYZE_SCRIPT}"
    return 0
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    echo "WARN: python3 not found; skipping verify batch summary."
    return 0
  fi

  cmd=(
    "${ANALYZE_SCRIPT}" batch-summary
    --latest "${latest_count}"
    --logs-root "${batch_logs_root}"
    --out "${out_file}"
  )

  if [[ -n "${CB2000_VERIFY_BATCH_CORRECT_COUNT:-}" ]]; then
    cmd+=(--correct-count "${CB2000_VERIFY_BATCH_CORRECT_COUNT}")
  fi

  if [[ -n "${CB2000_VERIFY_BATCH_CUTOFF_TIME:-}" ]]; then
    cmd+=(--cutoff-time "${CB2000_VERIFY_BATCH_CUTOFF_TIME}")
  fi

  echo
  echo "== Verify Batch Summary =="
  "${cmd[@]}" | tee "${LOG_DIR}/verify_batch_summary.log" || true

  cp -f "${RUN_DIR}/verify_batch_summary.txt" "${LATEST_DIR}/verify_batch_summary.txt" 2>/dev/null || true
  cp -f "${LOG_DIR}/verify_batch_summary.log" "${RUN_DIR}/verify_batch_summary.log" 2>/dev/null || true
  cp -f "${LOG_DIR}/verify_batch_summary.log" "${LATEST_DIR}/verify_batch_summary.log" 2>/dev/null || true
}

emit_post_verify_reports() {
  emit_verify_gate_summary
  emit_verify_metrics
  emit_verify_batch_summary
}

check_runtime_abi_compat() {
  local probe_bin="${EXAMPLES_DIR}/img-capture"
  local abi_log

  if [[ ! -x "${probe_bin}" ]]; then
    echo "ERROR: binary not found/executable: ${probe_bin}"
    return 1
  fi

  abi_log="$(ldd "${probe_bin}" 2>&1 || true)"

  if echo "${abi_log}" | grep -q "version \`LIBGUSB_.*' not found"; then
    cat <<MSG
ERROR: incompatibilidade de ABI detectada (libgusb).
Provavel causa: o build atual foi feito em outro ambiente e voce pulou recompilacao.

Detalhe:
${abi_log}

Acao recomendada:
1) recompilar no mesmo ambiente em que vai executar os testes;
2) evitar usar '--in-container' manualmente fora do container.
MSG
    return 1
  fi

  if echo "${abi_log}" | grep -q "not found"; then
    cat <<MSG
ERROR: dependencia dinamica ausente para ${probe_bin}.

Detalhe:
${abi_log}

Acao recomendada: recompilar/instalar dependencias no ambiente atual.
MSG
    return 1
  fi

  return 0
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

  if [[ "${rc}" == "11" ]]; then
    if [[ "${AUTO_FIX_USB}" == "1" ]]; then
      bash "${USB_ACCESS_HELPER}" --ensure
    elif prompt_yes_no_default_yes "CB2000 sem permissao de escrita no USB. Instalar regra do host agora?"; then
      bash "${USB_ACCESS_HELPER}" --ensure
    fi

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
      echo "ERROR: nenhum CanvasBio CB2000 foi detectado no barramento USB."
      return 1
      ;;
    11)
      cat <<EOF
ERROR: o CB2000 continua sem permissao de escrita.
Execute antes:
  bash "${USB_ACCESS_HELPER}" --ensure
EOF
      return 1
      ;;
    *)
      echo "WARN: falha inesperada na verificacao de acesso USB (rc=${rc})."
      return 1
      ;;
  esac
}

prompt_yes_no_default_yes() {
  local msg="$1"
  local ans
  read -r -p "${msg} [Y/n]: " ans
  ans="${ans:-Y}"
  [[ "${ans}" =~ ^[Yy]$ ]]
}

run_attempt() {
  local stage="$1"
  local bin="$2"
  local attempt="$3"
  local stage_log="${RUN_DIR}/${stage}_log.txt"
  local attempt_log="${RUN_DIR}/attempts/${stage}_attempt$(printf '%02d' "${attempt}").log"

  {
    echo
    echo "=== ${stage^^} attempt ${attempt} @ $(date --iso-8601=seconds) ==="
  } | tee -a "${stage_log}" | tee -a "${LOG_DIR}/${stage}.log" >/dev/null

  set +e
  (cd "${RUN_DIR}" && G_MESSAGES_DEBUG=all "${RUNNER[@]}" "${EXAMPLES_DIR}/${bin}") \
    2>&1 | tee "${attempt_log}" | tee -a "${stage_log}" | tee -a "${LOG_DIR}/${stage}.log"
  local rc=${PIPESTATUS[0]}
  local raw_rc=${rc}
  set -e

  # img-capture may return non-zero even after a successful capture completion.
  # Normalize this case to avoid false-negative UX in the interactive runner.
  if [[ "${stage}" == "capture" && "${rc}" -ne 0 ]]; then
    if grep -Eq "Device reported capture completion|Completing action FPI_DEVICE_ACTION_CAPTURE in idle!" "${attempt_log}"; then
      rc=0
      echo "[capture] normalized exit code ${raw_rc} -> ${rc} (capture completion detected in log)" \
        | tee -a "${stage_log}" | tee -a "${LOG_DIR}/${stage}.log"
    fi
  fi

  echo "${stage} attempt ${attempt} exit code: ${rc}" | tee -a "${stage_log}" | tee -a "${LOG_DIR}/${stage}.log"

  if [[ "${stage}" == "verify" ]]; then
    cp -f "${attempt_log}" "${RUN_DIR}/verify_log${attempt}.txt"
  fi

  copy_runtime_latest
}

loop_stage() {
  local stage="$1"
  local bin="$2"
  local has_next="$3"
  local attempt=1
  local choice

  while true; do
    run_attempt "${stage}" "${bin}" "${attempt}"

    if [[ "${has_next}" == "1" ]]; then
      while true; do
        read -r -p "[${stage}] (r) repetir, (n) proximo, (q) encerrar: " choice
        case "${choice}" in
          r|R)
            attempt=$((attempt + 1))
            break
            ;;
          n|N)
            return 0
            ;;
          q|Q)
            return 2
            ;;
          *)
            echo "Opcao invalida. Use r, n ou q."
            ;;
        esac
      done
    else
      while true; do
        read -r -p "[${stage}] (r) repetir, (q) encerrar: " choice
        case "${choice}" in
          r|R)
            attempt=$((attempt + 1))
            break
            ;;
          q|Q)
            return 2
            ;;
          *)
            echo "Opcao invalida. Use r ou q."
            ;;
        esac
      done
    fi
  done
}

run_build() {
  local meson_setup_args_raw="${CB2000_MESON_SETUP_ARGS:-${MESON_SETUP_ARGS_DEFAULT}}"
  local -a meson_setup_args=()

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

  echo "== Driver hash check ==" | tee -a "${LOG_DIR}/copy_driver.log"
  sha256sum "${DRIVER_SRC}" "${LIBFPRINT_DRIVER_DIR}/canvasbio_cb2000.c" | tee -a "${LOG_DIR}/copy_driver.log"
  sha256sum "${DRIVER_SIGFM_SRC}" "${LIBFPRINT_DRIVER_DIR}/cb2000_sigfm_matcher.c" | tee -a "${LOG_DIR}/copy_driver.log"
  sha256sum "${DRIVER_SIGFM_HDR}" "${LIBFPRINT_DRIVER_DIR}/cb2000_sigfm_matcher.h" | tee -a "${LOG_DIR}/copy_driver.log"

  if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    echo "== Configuring Meson builddir =="
    # shellcheck disable=SC2206
    meson_setup_args=(${meson_setup_args_raw})
    (
      cd "${LIBFPRINT_ROOT}" && \
      meson setup builddir "${meson_setup_args[@]}"
    ) 2>&1 | tee "${LOG_DIR}/meson_setup.log"
  fi

  echo "== Building libfprint =="
  (cd "${LIBFPRINT_ROOT}" && ninja -C builddir) 2>&1 | tee "${LOG_DIR}/build.log"
}

main() {
  local did_build=0

  echo "== CB2000 interactive session =="
  echo "session: ${SESSION_ID}"
  echo "run dir: ${RUN_DIR}"
  echo "latest : ${LATEST_DIR}"
  log_session_env_usage

  if prompt_yes_no_default_yes "Copiar driver e compilar antes dos testes?"; then
    run_build
    did_build=1
  else
    echo "Build pulado por escolha do usuario."
    if [[ ! -f "${SIGFM_OPENCV_HELPER_SO}" ]]; then
      if prompt_yes_no_default_yes "Helper OpenCV obrigatorio nao encontrado. Compilar helper agora?"; then
        if [[ ! -f "${SIGFM_OPENCV_HELPER_BUILD_SCRIPT}" ]]; then
          echo "ERROR: SIGFM OpenCV helper build script not found: ${SIGFM_OPENCV_HELPER_BUILD_SCRIPT}"
          exit 1
        fi
        echo "== Building SIGFM OpenCV helper (mandatory) =="
        bash "${SIGFM_OPENCV_HELPER_BUILD_SCRIPT}" 2>&1 | tee "${LOG_DIR}/build_sigfm_opencv_helper.log"
      else
        echo "ERROR: helper OpenCV obrigatorio ausente: ${SIGFM_OPENCV_HELPER_SO}"
        exit 1
      fi
    fi
  fi

  if [[ ! -f "${SIGFM_OPENCV_HELPER_SO}" ]]; then
    echo "ERROR: helper OpenCV obrigatorio ausente: ${SIGFM_OPENCV_HELPER_SO}"
    exit 1
  fi

  if ! check_runtime_abi_compat; then
    if [[ "${did_build}" -eq 0 ]] && prompt_yes_no_default_yes "Detectei incompatibilidade. Compilar agora para corrigir?"; then
      run_build
      did_build=1
      check_runtime_abi_compat
    else
      exit 1
    fi
  fi

  if ! ensure_usb_access; then
    exit 1
  fi

  snapshot_used_driver_files

  echo "\n== Stage 1: capture =="
  set +e
  loop_stage "capture" "img-capture" "1"
  stage_rc=$?
  set -e
  if [[ "${stage_rc}" -eq 2 ]]; then
    echo "Sessao encerrada pelo usuario durante capture."
    copy_runtime_latest
    emit_post_verify_reports
    exit 0
  elif [[ "${stage_rc}" -ne 0 ]]; then
    echo "Erro inesperado na fase capture (rc=${stage_rc})."
    exit "${stage_rc}"
  fi

  echo "\n== Stage 2: enroll =="
  echo "Dica de enroll (A1.2): varie levemente posicao/rotacao e use pressao leve-media entre capturas."
  echo "Evite amostras quase identicas e evite deslocamentos extremos."
  set +e
  loop_stage "enroll" "enroll" "1"
  stage_rc=$?
  set -e
  if [[ "${stage_rc}" -eq 2 ]]; then
    echo "Sessao encerrada pelo usuario durante enroll."
    copy_runtime_latest
    emit_post_verify_reports
    exit 0
  elif [[ "${stage_rc}" -ne 0 ]]; then
    echo "Erro inesperado na fase enroll (rc=${stage_rc})."
    exit "${stage_rc}"
  fi

  echo "\n== Stage 3: verify =="
  set +e
  loop_stage "verify" "verify" "0"
  stage_rc=$?
  set -e
  if [[ "${stage_rc}" -eq 2 ]]; then
    echo "Sessao encerrada pelo usuario durante verify."
    copy_runtime_latest
    emit_post_verify_reports
    exit 0
  elif [[ "${stage_rc}" -ne 0 ]]; then
    echo "Erro inesperado na fase verify (rc=${stage_rc})."
    exit "${stage_rc}"
  fi

  copy_runtime_latest
  emit_post_verify_reports
  echo "== Sessao finalizada =="
  echo "run dir: ${RUN_DIR}"
  echo "latest : ${LATEST_DIR}"
}

main "$@"
