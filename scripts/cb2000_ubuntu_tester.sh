#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DBX_NAME="${DBX_NAME:-canvasbio}"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-docker.io/library/ubuntu:24.04}"
ACTION="${1:-}"

show_help() {
  cat <<EOF
Uso: $(basename "$0") [acao]

Acoes:
  prepare-host     instala/atualiza a regra udev do host para o CB2000
  prepare-runtime  cria o container Ubuntu, instala deps e compila o ambiente
  test             executa o runner automatico
  interactive      executa a sessao interativa
  build-deb        gera o pacote .deb tester
  all              prepare-host + prepare-runtime + build-deb
EOF
}

ensure_container() {
  if ! distrobox list --no-color 2>/dev/null | grep -qw "${DBX_NAME}"; then
    echo "== Criando container ${DBX_NAME} (${CONTAINER_IMAGE}) =="
    distrobox create --image "${CONTAINER_IMAGE}" --name "${DBX_NAME}" --yes
  fi
}

run_in_container() {
  distrobox enter "${DBX_NAME}" -- /bin/bash -lc "cd '${PROJECT_ROOT}' && $*"
}

prepare_host() {
  bash "${PROJECT_ROOT}/scripts/cb2000_usb_host_access.sh" --ensure
}

prepare_runtime() {
  ensure_container
  run_in_container "bash scripts/setup-libfprint.sh --install-deps --clone --integrate --build"
  run_in_container "bash scripts/build_sigfm_opencv_helper.sh"
}

run_test() {
  prepare_host
  ensure_container
  run_in_container "env CB2000_AUTO_FIX_USB=1 bash scripts/run_cb2000_test.sh --in-container"
}

run_interactive() {
  prepare_host
  ensure_container
  run_in_container "env CB2000_AUTO_FIX_USB=1 bash scripts/run_cb2000_interactive.sh --in-container"
}

build_deb() {
  ensure_container
  run_in_container "bash scripts/build_libfprint_deb_ubuntu.sh --in-container"
}

case "${ACTION}" in
  prepare-host)
    prepare_host
    ;;
  prepare-runtime)
    prepare_runtime
    ;;
  test)
    run_test
    ;;
  interactive)
    run_interactive
    ;;
  build-deb)
    build_deb
    ;;
  all)
    prepare_host
    prepare_runtime
    build_deb
    ;;
  -h|--help|"")
    show_help
    ;;
  *)
    echo "Unknown action: ${ACTION}"
    show_help
    exit 1
    ;;
esac
