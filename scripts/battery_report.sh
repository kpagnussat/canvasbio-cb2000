#!/usr/bin/env bash
# battery_report.sh — GAR/FAR report for a test session.
# Usage: ./battery_report.sh [--session SESSION] [--cut HH:MM:SS] [--sigfm]
# See: scripts/README.md for full documentation.

set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="${PROJECT_ROOT}/scripts/venv"
ENV_FILE="${PROJECT_ROOT}/.cb2000_env"

# Load configuration saved by the orchestrator (CB2000_LOG_ROOT, BUILD_TARGET, etc.)
if [[ -f "${ENV_FILE}" ]]; then
  # shellcheck source=/dev/null
  source "${ENV_FILE}"
fi
# Portable default if not set
export CB2000_LOG_ROOT="${CB2000_LOG_ROOT:-${PROJECT_ROOT}/../dev_logs/sessions}"

if [[ -f "${VENV_DIR}/bin/activate" ]]; then
  # shellcheck source=/dev/null
  source "${VENV_DIR}/bin/activate"
fi

exec python3 "${PROJECT_ROOT}/scripts/cb2000_battery_report.py" "$@"
