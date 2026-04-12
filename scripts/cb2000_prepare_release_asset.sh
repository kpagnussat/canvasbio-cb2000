#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DISTRO_KEY=""
PACKAGE_NAME=""
PUBLIC_NAME=""
ARTIFACT_PATH=""
SNAPSHOT_TAG="${CB2000_RELEASE_SNAPSHOT:-R2.5}"
TARGET_ROOT=""

show_help() {
  cat <<'EOF'
Usage: ./scripts/cb2000_prepare_release_asset.sh \
  --distro <key> \
  --package-name <name> \
  [--public-name <name>] \
  --artifact <path> \
  [--snapshot <tag>] \
  [--target-root <dir>]

Creates a normalized public-facing release attachment alias from a distro-native
package build output.

If `--public-name` is omitted, the distro-native package name is reused.

Examples:
  ./scripts/cb2000_prepare_release_asset.sh \
    --distro ubuntu-debian \
    --package-name libfprint-2-2-canvasbio \
    --public-name libfprint-canvasbio-cb2000 \
    --artifact "$HOME/libfprint-deb-build/libfprint-2-2-canvasbio_1.94.10+canvasbio.202604121230_amd64.deb"

  ./scripts/cb2000_prepare_release_asset.sh \
    --distro fedora \
    --package-name libfprint \
    --public-name libfprint-canvasbio-cb2000 \
    --artifact "$HOME/rpmbuild/RPMS/x86_64/libfprint-1.94.10-99.canvasbio.202604121232.fc43.x86_64.rpm"
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --distro)
      DISTRO_KEY="${2:-}"
      shift 2
      ;;
    --package-name)
      PACKAGE_NAME="${2:-}"
      shift 2
      ;;
    --public-name)
      PUBLIC_NAME="${2:-}"
      shift 2
      ;;
    --artifact)
      ARTIFACT_PATH="${2:-}"
      shift 2
      ;;
    --snapshot)
      SNAPSHOT_TAG="${2:-}"
      shift 2
      ;;
    --target-root)
      TARGET_ROOT="${2:-}"
      shift 2
      ;;
    -h|--help)
      show_help
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      show_help >&2
      exit 1
      ;;
  esac
done

if [[ -z "${DISTRO_KEY}" || -z "${PACKAGE_NAME}" || -z "${ARTIFACT_PATH}" ]]; then
  echo "ERROR: --distro, --package-name and --artifact are required." >&2
  show_help >&2
  exit 1
fi

if [[ -z "${PUBLIC_NAME}" ]]; then
  PUBLIC_NAME="${PACKAGE_NAME}"
fi

if [[ ! -f "${ARTIFACT_PATH}" ]]; then
  echo "ERROR: artifact not found: ${ARTIFACT_PATH}" >&2
  exit 1
fi

case "${ARTIFACT_PATH}" in
  *.pkg.tar.zst)
    EXTENSION="pkg.tar.zst"
    ;;
  *.deb)
    EXTENSION="deb"
    ;;
  *.rpm)
    EXTENSION="rpm"
    ;;
  *)
    echo "ERROR: unsupported artifact extension: ${ARTIFACT_PATH}" >&2
    exit 1
    ;;
esac

if [[ -z "${TARGET_ROOT}" ]]; then
  TARGET_ROOT="${PROJECT_ROOT}/test/release_assets/${SNAPSHOT_TAG}/public"
fi

mkdir -p "${TARGET_ROOT}"

TARGET_FILE="${TARGET_ROOT}/${DISTRO_KEY}_${PUBLIC_NAME}.${EXTENSION}"
cp -f "${ARTIFACT_PATH}" "${TARGET_FILE}"

MANIFEST_PATH="${TARGET_ROOT}/manifest.txt"
TMP_MANIFEST="$(mktemp)"
{
  printf 'snapshot=%s\n' "${SNAPSHOT_TAG}"
  printf 'generated_at=%s\n' "$(date --iso-8601=seconds)"
  printf '\n'

  find "${TARGET_ROOT}" -maxdepth 1 -type f \
    ! -name 'manifest.txt' \
    -printf '%f\n' | sort | while read -r f; do
      sha="$(sha256sum "${TARGET_ROOT}/${f}" | awk '{print $1}')"
      printf '%s  %s\n' "${sha}" "${f}"
    done
} > "${TMP_MANIFEST}"
mv -f "${TMP_MANIFEST}" "${MANIFEST_PATH}"

echo "Normalized release asset ready:"
echo "  source : ${ARTIFACT_PATH}"
echo "  target : ${TARGET_FILE}"
echo "  sha256 : $(sha256sum "${TARGET_FILE}" | awk '{print $1}')"
