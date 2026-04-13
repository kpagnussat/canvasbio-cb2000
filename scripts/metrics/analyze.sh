#!/usr/bin/env bash
# analyze.sh — CB2000 metrics and reporting tool.
#
# Usage: ./scripts/metrics/analyze.sh <command> [options]
#
# Commands:
#   run-report    --run <run_dir>   Full per-run telemetry and verify report
#   gate-summary  --run <run_dir>   Finalize-ACK and ready-matrix routing summary
#   metrics       --run <run_dir>   Matcher decision counts and rates
#   enroll-gate   <run_dir>         Enrollment gate diagnostics and diversity analysis
#   fn-rootcause  --run <run_dir>   Root-cause classifier for false negatives
#   batch-summary                   Cross-run batch verify summary table
#   battery       [options]         GAR/FAR session battery report
#   all           --run <run_dir>   Run all per-run reports in sequence, then cross-run
#
# See: scripts/README.md for full documentation.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VENV_DIR="${PROJECT_ROOT}/scripts/venv"
ENV_FILE="${PROJECT_ROOT}/.cb2000_env"

if [[ -f "${ENV_FILE}" ]]; then
  # shellcheck source=/dev/null
  source "${ENV_FILE}"
fi
export CB2000_LOG_ROOT="${CB2000_LOG_ROOT:-${PROJECT_ROOT}/../dev_logs/sessions}"

if [[ -f "${VENV_DIR}/bin/activate" ]]; then
  # shellcheck source=/dev/null
  source "${VENV_DIR}/bin/activate"
fi

CMD="${1:-}"
shift || true

case "${CMD}" in
  run-report)
    exec python3 "${SCRIPT_DIR}/cb2000_run_report.py" "$@"
    ;;
  gate-summary)
    exec python3 "${SCRIPT_DIR}/cb2000_verify_gate_summary.py" "$@"
    ;;
  metrics)
    exec python3 "${SCRIPT_DIR}/cb2000_verify_metrics.py" "$@"
    ;;
  enroll-gate)
    exec python3 "${SCRIPT_DIR}/cb2000_enroll_gate_report.py" "$@"
    ;;
  fn-rootcause)
    exec python3 "${SCRIPT_DIR}/cb2000_verify_fn_rootcause.py" "$@"
    ;;
  batch-summary)
    exec python3 "${SCRIPT_DIR}/cb2000_verify_batch_summary.py" "$@"
    ;;
  battery)
    exec python3 "${SCRIPT_DIR}/cb2000_battery_report.py" "$@"
    ;;
  all)
    # Parse --run <dir> from remaining args; pass remaining flags to each script.
    RUN_DIR=""
    OTHER_ARGS=()
    while [[ $# -gt 0 ]]; do
      if [[ "$1" == "--run" && -n "${2:-}" ]]; then
        RUN_DIR="$2"; shift 2
      else
        OTHER_ARGS+=("$1"); shift
      fi
    done
    if [[ -z "${RUN_DIR}" ]]; then
      echo "analyze.sh all: --run <run_dir> is required" >&2; exit 1
    fi
    python3 "${SCRIPT_DIR}/cb2000_run_report.py"           --run "${RUN_DIR}" "${OTHER_ARGS[@]+"${OTHER_ARGS[@]}"}"
    python3 "${SCRIPT_DIR}/cb2000_verify_gate_summary.py"  --run "${RUN_DIR}" "${OTHER_ARGS[@]+"${OTHER_ARGS[@]}"}"
    python3 "${SCRIPT_DIR}/cb2000_verify_metrics.py"       --run "${RUN_DIR}" "${OTHER_ARGS[@]+"${OTHER_ARGS[@]}"}"
    python3 "${SCRIPT_DIR}/cb2000_enroll_gate_report.py"   "${RUN_DIR}"       "${OTHER_ARGS[@]+"${OTHER_ARGS[@]}"}" || true
    python3 "${SCRIPT_DIR}/cb2000_verify_fn_rootcause.py"  --run "${RUN_DIR}" "${OTHER_ARGS[@]+"${OTHER_ARGS[@]}"}" || true
    python3 "${SCRIPT_DIR}/cb2000_verify_batch_summary.py"                    "${OTHER_ARGS[@]+"${OTHER_ARGS[@]}"}" || true
    python3 "${SCRIPT_DIR}/cb2000_battery_report.py"                          "${OTHER_ARGS[@]+"${OTHER_ARGS[@]}"}" || true
    ;;
  ""|--help|-h)
    grep "^#" "$0" | grep -v "^#!/" | sed 's/^# \?//'
    exit 0
    ;;
  *)
    echo "Unknown command: ${CMD}" >&2
    echo "Run: $(basename "$0") --help" >&2
    exit 1
    ;;
esac
