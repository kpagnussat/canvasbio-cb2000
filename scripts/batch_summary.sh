#!/usr/bin/env bash
# batch_summary.sh — Summary of multiple verify attempts in a batch run.
# Usage: ./batch_summary.sh --run RUN_DIR [options]
# See: scripts/README.md for full documentation.

set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="${PROJECT_ROOT}/scripts/venv"
ENV_FILE="${PROJECT_ROOT}/.cb2000_env"

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

exec python3 "${PROJECT_ROOT}/scripts/cb2000_verify_batch_summary.py" "$@"
