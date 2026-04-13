#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RULE_SRC="${PROJECT_ROOT}/tools/99-canvasbio.rules"
RULE_DEST="/etc/udev/rules.d/99-canvasbio.rules"
QUIET=0
ACTION="check"

host_exec() {
  if command -v distrobox-host-exec >/dev/null 2>&1; then
    distrobox-host-exec "$@"
  else
    "$@"
  fi
}

host_sudo() {
  if command -v distrobox-host-exec >/dev/null 2>&1; then
    distrobox-host-exec sudo "$@"
  else
    sudo "$@"
  fi
}

find_cb2000_nodes() {
  local dev_dir vendor product busnum devnum devnode
  for dev_dir in /sys/bus/usb/devices/*; do
    [[ -f "${dev_dir}/idVendor" ]] || continue
    vendor="$(tr '[:upper:]' '[:lower:]' < "${dev_dir}/idVendor")"
    [[ "${vendor}" == "2df0" ]] || continue
    product="$(tr '[:upper:]' '[:lower:]' < "${dev_dir}/idProduct")"
    busnum="$(< "${dev_dir}/busnum")"
    devnum="$(< "${dev_dir}/devnum")"
    printf -v devnode "/dev/bus/usb/%03d/%03d" "${busnum}" "${devnum}"
    printf '%s|%s|%s\n' "${product}" "${devnode}" "${dev_dir}"
  done
}

print_status() {
  local found=0 line product devnode dev_dir

  while IFS='|' read -r product devnode dev_dir; do
    [[ -n "${devnode}" ]] || continue
    found=1
    if [[ -w "${devnode}" ]]; then
      [[ "${QUIET}" == "1" ]] || echo "OK: CB2000 ${product} acessivel em ${devnode}"
    else
      [[ "${QUIET}" == "1" ]] || {
        echo "WARN: CB2000 ${product} detectado em ${devnode}, mas sem permissao de escrita para o usuario atual."
        ls -l "${devnode}" 2>/dev/null || true
      }
    fi
  done < <(find_cb2000_nodes)

  if [[ "${found}" == "0" ]]; then
    [[ "${QUIET}" == "1" ]] || echo "WARN: nenhum dispositivo CanvasBio CB2000 foi detectado no barramento USB."
    return 10
  fi

  while IFS='|' read -r _product devnode _dev_dir; do
    [[ -n "${devnode}" ]] || continue
    if [[ ! -w "${devnode}" ]]; then
      return 11
    fi
  done < <(find_cb2000_nodes)

  return 0
}

reload_host_rules() {
  echo "== Reloading udev rules =="
  host_sudo udevadm control --reload-rules

  # Trigger by sysfs device path. --attr-match filtering is unreliable for
  # re-applying MODE changes to already-enumerated devices; triggering by path
  # with --action=change is the only form that consistently re-applies rules.
  local triggered=0 product devnode dev_dir
  while IFS='|' read -r product devnode dev_dir; do
    [[ -n "${dev_dir}" ]] || continue
    host_sudo udevadm trigger --action=change "${dev_dir}" || true
    echo "Triggered: ${dev_dir} (${devnode})"
    triggered=1
  done < <(find_cb2000_nodes)

  if [[ "${triggered}" == "0" ]]; then
    echo "No CB2000 device found; triggering full usb subsystem as fallback."
    host_sudo udevadm trigger --action=change --subsystem-match=usb || true
  fi

  echo "Rules reloaded."
}

is_host_rule_installed() {
  # Inside a container /run/host mirrors the host root; outside, use direct path.
  if [[ -d /run/host/etc/udev/rules.d ]]; then
    [[ -f "/run/host${RULE_DEST}" ]]
  else
    host_exec test -f "${RULE_DEST}" 2>/dev/null
  fi
}

install_host_rule() {
  if [[ ! -f "${RULE_SRC}" ]]; then
    echo "ERROR: regra udev nao encontrada: ${RULE_SRC}"
    exit 1
  fi

  if is_host_rule_installed; then
    echo "== udev rule already installed; reloading only =="
    reload_host_rules
  else
    echo "== Installing CanvasBio udev rule on host =="
    host_sudo install -D -m 0644 "${RULE_SRC}" "${RULE_DEST}"
    echo "Rule installed at ${RULE_DEST}"
    reload_host_rules
  fi
}

show_help() {
  cat <<EOF
Usage: $(basename "$0") [--check | --install-host-rule | --ensure | --reload-rules] [--quiet]

Actions:
  --check               Check if CB2000 is visible and writable
  --install-host-rule   Install tools/99-canvasbio.rules on host (skips copy if already present)
  --ensure              Install rule if missing and revalidate; if already installed, reload only
  --reload-rules        Reload udev rules and trigger only, without copying the rule file
  --quiet               Suppress informational output; rely on return code only

Return codes for --check:
  0   device present and writable
  10  device not found
  11  device found but not writable
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check)
      ACTION="check"
      ;;
    --install-host-rule)
      ACTION="install"
      ;;
    --ensure)
      ACTION="ensure"
      ;;
    --reload-rules)
      ACTION="reload"
      ;;
    --quiet)
      QUIET=1
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

case "${ACTION}" in
  check)
    print_status
    ;;
  install)
    install_host_rule
    ;;
  reload)
    reload_host_rules
    ;;
  ensure)
    set +e
    print_status
    status=$?
    set -e
    if [[ "${status}" == "11" ]]; then
      install_host_rule
      print_status
    else
      exit "${status}"
    fi
    ;;
esac
