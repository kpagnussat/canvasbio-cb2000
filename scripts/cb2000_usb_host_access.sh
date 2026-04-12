#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
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

install_host_rule() {
  if [[ ! -f "${RULE_SRC}" ]]; then
    echo "ERROR: regra udev nao encontrada: ${RULE_SRC}"
    exit 1
  fi

  echo "== Instalando regra udev do CanvasBio no host =="
  host_sudo install -D -m 0644 "${RULE_SRC}" "${RULE_DEST}"
  host_sudo udevadm control --reload-rules
  host_sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=2df0 || true
  echo "Regra instalada em ${RULE_DEST}"
}

show_help() {
  cat <<EOF
Uso: $(basename "$0") [--check | --install-host-rule | --ensure] [--quiet]

Accoes:
  --check               Verifica se o CB2000 esta visivel e gravavel
  --install-host-rule   Instala tools/99-canvasbio.rules no host
  --ensure              Instala a regra se necessario e revalida
  --quiet               Suprime saida informativa; usa apenas o codigo de retorno

Codigos de retorno do --check:
  0   dispositivo presente e gravavel
  10  dispositivo nao encontrado
  11  dispositivo encontrado, mas sem permissao de escrita
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
