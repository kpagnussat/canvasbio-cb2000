#!/usr/bin/env bash
# Build a custom libfprint package for Arch Linux using makepkg.
#
# By default runs inside a persistent distrobox container (created on first run).
# Use --ephemeral for a throwaway container, --in-container if already inside.
#
# Usage:
#   ./scripts/build_libfprint_pkg_arch.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]
#
# Flags:
#   (none)              Auto: enter persistent container, create it if needed
#   --in-container      Already inside the correct container — run build directly
#   --no-container      Run build directly on the host (no distrobox required)
#   --ephemeral         Use a throwaway container (no state saved between runs)
#   --persistent        Use/create a named persistent distrobox container (default)
#   --create-container  Create the persistent container only, do not build
#   -h, --help          Show this help
#
# Environment variables:
#   LIBFPRINT_DIR    Local libfprint source tree (default: $HOME/libfprint)
#   BUILD_DIR        Working build directory (default: $HOME/libfprint-arch-build)
#   DBX_NAME         Container name (default: cb2000-arch)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Container config ──────────────────────────────────────────────────────────
CONTAINER_IMAGE="${CONTAINER_IMAGE:-docker.io/library/archlinux:latest}"
DBX_NAME="${DBX_NAME:-cb2000-arch}"

# ── Build paths ───────────────────────────────────────────────────────────────
LIBFPRINT_DIR="${LIBFPRINT_DIR:-${HOME}/libfprint}"
BUILD_DIR="${BUILD_DIR:-${HOME}/libfprint-arch-build}"
TIMESTAMP=$(date +%Y%m%d%H%M)
PKG_REL="${TIMESTAMP}"

# ── Colors ────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
else
    GREEN=''; RED=''; YELLOW=''; NC=''
fi

show_help() {
    grep "^#" "$0" | grep -v "^#!/" | sed 's/^# \?//'
}

install_driver_into_tree() {
    local target_tree="$1"
    local driver_dir="${target_tree}/libfprint/drivers/canvasbio_cb2000"
    local libfprint_meson="${target_tree}/libfprint/meson.build"
    local top_meson="${target_tree}/meson.build"

    mkdir -p "${driver_dir}"
    cp -f "${PROJECT_ROOT}/src/canvasbio_cb2000.c" "${driver_dir}/canvasbio_cb2000.c"
    cp -f "${PROJECT_ROOT}/src/cb2000_sigfm_matcher.c" "${driver_dir}/cb2000_sigfm_matcher.c"
    cp -f "${PROJECT_ROOT}/src/cb2000_sigfm_matcher.h" "${driver_dir}/cb2000_sigfm_matcher.h"
    cp -f "${PROJECT_ROOT}/src/meson.build" "${driver_dir}/meson.build"

    if ! grep -q "'canvasbio_cb2000'" "${libfprint_meson}"; then
        perl -0pi -e "s/'focaltech_moc'\\s*:\\s*\\n\\s*\\[ 'drivers\\/focaltech_moc\\/focaltech_moc\\.c' \\],\\n}/'focaltech_moc' :\\n        [ 'drivers\\/focaltech_moc\\/focaltech_moc.c' ],\\n    'canvasbio_cb2000' :\\n        [ 'drivers\\/canvasbio_cb2000\\/canvasbio_cb2000.c', 'drivers\\/canvasbio_cb2000\\/cb2000_sigfm_matcher.c' ],\\n}/s" "${libfprint_meson}"
    fi

    if ! grep -q "'canvasbio_cb2000'" "${top_meson}"; then
        perl -0pi -e "s/'focaltech_moc',\\n]/'focaltech_moc',\\n    'canvasbio_cb2000',\\n]/s" "${top_meson}"
    fi
}

# ── Container management ──────────────────────────────────────────────────────
_enter_container() {
    local ephemeral="${1:-0}"
    local self; self="$(realpath "$0")"
    local env_fwd="LIBFPRINT_DIR='${LIBFPRINT_DIR}' BUILD_DIR='${BUILD_DIR}' DBX_NAME='${DBX_NAME}'"

    if [[ "${ephemeral}" == "1" ]]; then
        echo -e "${YELLOW}>>> Ephemeral container: ${CONTAINER_IMAGE}${NC}"
        if command -v distrobox &>/dev/null && distrobox ephemeral --help &>/dev/null 2>&1; then
            exec distrobox ephemeral --image "${CONTAINER_IMAGE}" -- \
                /bin/bash -lc "${env_fwd} '${self}' --in-container"
        elif command -v podman &>/dev/null; then
            exec podman run --rm -it \
                -v "${PROJECT_ROOT}:${PROJECT_ROOT}:z" \
                -v "${HOME}:${HOME}:z" \
                -e "LIBFPRINT_DIR=${LIBFPRINT_DIR}" \
                -e "BUILD_DIR=${BUILD_DIR}" \
                -e "DBX_NAME=${DBX_NAME}" \
                "${CONTAINER_IMAGE}" \
                /bin/bash -lc "'${self}' --in-container"
        else
            echo -e "${RED}ERROR: neither distrobox-ephemeral nor podman found.${NC}"
            exit 1
        fi
    else
        if ! distrobox list --no-color 2>/dev/null | grep -qw "${DBX_NAME}"; then
            echo -e "${YELLOW}>>> Creating container '${DBX_NAME}' (${CONTAINER_IMAGE})...${NC}"
            distrobox create --image "${CONTAINER_IMAGE}" --name "${DBX_NAME}" --yes
        fi
        exec distrobox enter "${DBX_NAME}" -- \
            /bin/bash -lc "${env_fwd} '${self}' --in-container"
    fi
}

_MODE="${1:-}"

case "${_MODE}" in
    --in-container|--no-container)
        : # fall through to build logic below
        ;;
    --ephemeral|-e)
        _enter_container 1
        ;;
    --create-container)
        if ! distrobox list --no-color 2>/dev/null | grep -qw "${DBX_NAME}"; then
            echo -e "${YELLOW}>>> Creating container '${DBX_NAME}' (${CONTAINER_IMAGE})...${NC}"
            distrobox create --image "${CONTAINER_IMAGE}" --name "${DBX_NAME}" --yes
            echo -e "${GREEN}>>> Container created. Run without --create-container to build.${NC}"
        else
            echo -e "${GREEN}>>> Container '${DBX_NAME}' already exists.${NC}"
        fi
        exit 0
        ;;
    ""|--persistent|-p)
        if [[ "${CONTAINER_ID:-}" == "${DBX_NAME}" ]]; then
            : # already inside the correct container
        elif [[ -n "${CONTAINER_ID:-}" ]]; then
            if command -v distrobox-host-exec &>/dev/null; then
                exec distrobox-host-exec "$0" "${@}"
            fi
            echo -e "${RED}ERROR: inside container '${CONTAINER_ID}' but distrobox-host-exec not available.${NC}"
            exit 1
        else
            _enter_container 0
        fi
        ;;
    -h|--help)
        show_help; exit 0
        ;;
    *)
        echo "Unknown option: ${_MODE}"; show_help; exit 1
        ;;
esac

# ── Build (runs inside the container) ────────────────────────────────────────
echo -e "${YELLOW}>>> Checking Arch Linux environment...${NC}"

if ! grep -qi "arch" /etc/os-release 2>/dev/null; then
    echo -e "${RED}ERROR: This script requires Arch Linux.${NC}"
    exit 1
fi

echo -e "${GREEN}>>> Installing build dependencies...${NC}"
sudo pacman -Syu --noconfirm --needed \
    base-devel meson ninja pkg-config \
    glib2 glib2-devel libgusb pixman nss \
    libgudev cairo libusb git \
    opencv

if [[ ! -d "${LIBFPRINT_DIR}" ]]; then
    echo -e "${YELLOW}>>> Cloning libfprint...${NC}"
    git clone https://gitlab.freedesktop.org/libfprint/libfprint.git "${LIBFPRINT_DIR}"
fi

echo -e "${YELLOW}>>> Syncing CanvasBio driver into libfprint tree...${NC}"
install_driver_into_tree "${LIBFPRINT_DIR}"

# Detect version from upstream meson.build
PKG_VERSION=$(grep -E "^\s*version\s*:" "${LIBFPRINT_DIR}/meson.build" 2>/dev/null \
    | head -1 | sed "s/.*version\s*:\s*'//;s/'.*//" | tr -d ' ')
[[ -z "${PKG_VERSION}" ]] && PKG_VERSION="1.94.8"

echo -e "${YELLOW}>>> Building OpenCV sidecar...${NC}"
bash "${PROJECT_ROOT}/scripts/packaging/build_sigfm_opencv_helper.sh"
SIDECAR_SO="${PROJECT_ROOT}/build/libcb2000_sigfm_opencv.so"

echo -e "${YELLOW}>>> Preparing build at ${BUILD_DIR} (version ${PKG_VERSION})...${NC}"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

# Create tarball from local source
TARBALL="${BUILD_DIR}/libfprint-${PKG_VERSION}.tar.gz"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DIR}"' EXIT

cp -r "${LIBFPRINT_DIR}" "${TMP_DIR}/libfprint-${PKG_VERSION}"
rm -rf "${TMP_DIR}/libfprint-${PKG_VERSION}/.git"
install_driver_into_tree "${TMP_DIR}/libfprint-${PKG_VERSION}"
tar -C "${TMP_DIR}" -czf "${TARBALL}" "libfprint-${PKG_VERSION}"

# Generate PKGBUILD
cat > "${BUILD_DIR}/PKGBUILD" <<PKGBUILD
# Maintainer: CanvasBio Build <canvasbio@local>
pkgname=libfprint-canvasbio
pkgver=${PKG_VERSION}
pkgrel=${PKG_REL}
pkgdesc="libfprint with CanvasBio CB2000 driver (custom build ${TIMESTAMP})"
arch=('x86_64' 'aarch64')
url="https://gitlab.freedesktop.org/libfprint/libfprint"
license=('LGPL-2.1-or-later' 'MIT')
depends=('glib2' 'nss' 'libgusb' 'pixman' 'libgudev' 'cairo' 'libusb' 'opencv')
makedepends=('meson' 'ninja' 'pkg-config' 'gtk-doc' 'gobject-introspection' 'glib2-devel')
provides=('libfprint' 'libfprint-tod')
conflicts=('libfprint' 'libfprint-tod')
source=("libfprint-\${pkgver}.tar.gz"
        "libcb2000_sigfm_opencv.so"
        "99-canvasbio.rules"
        "50-canvasbio-fprint.rules"
        "README.canvasbio.md"
        "LICENSE.canvasbio"
        "COPYRIGHT.canvasbio"
        "THIRD_PARTY_NOTICES.canvasbio.md"
        "SIGFM-MIT.txt")
install=libfprint-canvasbio.install
sha256sums=('SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP')

build() {
  cd "libfprint-\${pkgver}"
  arch-meson . build -Ddoc=false -Dgtk-examples=false
  ninja -C build
}

package() {
  cd "libfprint-\${pkgver}"
  DESTDIR="\${pkgdir}" ninja -C build install
  install -Dm755 "\${srcdir}/libcb2000_sigfm_opencv.so" \
      "\${pkgdir}/usr/lib/libcb2000_sigfm_opencv.so"
  install -Dm644 "\${srcdir}/99-canvasbio.rules" \
      "\${pkgdir}/etc/udev/rules.d/99-canvasbio.rules"
  install -Dm644 "\${srcdir}/50-canvasbio-fprint.rules" \
      "\${pkgdir}/etc/polkit-1/rules.d/50-canvasbio-fprint.rules"
  install -Dm644 "\${srcdir}/README.canvasbio.md" \
      "\${pkgdir}/usr/share/doc/\${pkgname}/README.md"
  install -Dm644 "\${srcdir}/LICENSE.canvasbio" \
      "\${pkgdir}/usr/share/doc/\${pkgname}/LICENSE"
  install -Dm644 "\${srcdir}/COPYRIGHT.canvasbio" \
      "\${pkgdir}/usr/share/doc/\${pkgname}/COPYRIGHT"
  install -Dm644 "\${srcdir}/THIRD_PARTY_NOTICES.canvasbio.md" \
      "\${pkgdir}/usr/share/doc/\${pkgname}/THIRD_PARTY_NOTICES.md"
  install -Dm644 "\${srcdir}/SIGFM-MIT.txt" \
      "\${pkgdir}/usr/share/doc/\${pkgname}/SIGFM-MIT.txt"

  # Release assets should carry runtime payload only.
  rm -rf \
      "\${pkgdir}/usr/include" \
      "\${pkgdir}/usr/lib/installed-tests" \
      "\${pkgdir}/usr/share/gir-1.0" \
      "\${pkgdir}/usr/share/installed-tests"
  rm -f \
      "\${pkgdir}/usr/lib/libfprint-2.so" \
      "\${pkgdir}/usr/lib/pkgconfig/libfprint-2.pc"
  rmdir --ignore-fail-on-non-empty "\${pkgdir}/usr/lib/pkgconfig" 2>/dev/null || true
}
PKGBUILD

cat > "${BUILD_DIR}/libfprint-canvasbio.install" <<'INSTALL'
post_install() {
    ldconfig
    # Trigger by subsystem only; --attr-match filtering is unreliable for
    # re-applying MODE changes to already-enumerated devices.
    udevadm control --reload-rules || true
    udevadm trigger --action=change --subsystem-match=usb || true
}
post_upgrade() { post_install; }
INSTALL

cp "${SIDECAR_SO}" "${BUILD_DIR}/libcb2000_sigfm_opencv.so"
install -m 0644 "${PROJECT_ROOT}/tools/99-canvasbio.rules" "${BUILD_DIR}/99-canvasbio.rules"
install -m 0644 "${PROJECT_ROOT}/README.md" "${BUILD_DIR}/README.canvasbio.md"
install -m 0644 "${PROJECT_ROOT}/LICENSE" "${BUILD_DIR}/LICENSE.canvasbio"
install -m 0644 "${PROJECT_ROOT}/COPYRIGHT" "${BUILD_DIR}/COPYRIGHT.canvasbio"
install -m 0644 "${PROJECT_ROOT}/THIRD_PARTY_NOTICES.md" "${BUILD_DIR}/THIRD_PARTY_NOTICES.canvasbio.md"
install -m 0644 "${PROJECT_ROOT}/third_party_licenses/SIGFM-MIT.txt" "${BUILD_DIR}/SIGFM-MIT.txt"
cat > "${BUILD_DIR}/50-canvasbio-fprint.rules" <<'POLKIT'
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("net.reactivated.fprint") === 0 && subject.local) {
        return polkit.Result.YES;
    }
});
POLKIT

echo -e "${GREEN}>>> Building Arch package (.pkg.tar.zst)...${NC}"
cd "${BUILD_DIR}"
makepkg -sf --noconfirm --skipinteg

PKG_FILE=$(find "${BUILD_DIR}" -maxdepth 1 \
    -name "libfprint-canvasbio-${PKG_VERSION}-${PKG_REL}-*.pkg.tar.zst" \
    | grep -v -- "-debug-" \
    | head -1)

if [[ -z "${PKG_FILE}" ]]; then
    echo -e "${RED}ERROR: package not found after build.${NC}"
    exit 1
fi

echo -e "\n${GREEN}========================================================${NC}"
echo -e "${GREEN}SUCCESS: $(basename "${PKG_FILE}")${NC}"
echo -e "${GREEN}========================================================${NC}"
echo -e "Output: ${PKG_FILE}"
bash "${PROJECT_ROOT}/scripts/cb2000_prepare_release_asset.sh" \
    --distro arch \
    --package-name libfprint-canvasbio \
    --public-name libfprint-canvasbio-cb2000 \
    --artifact "${PKG_FILE}"
echo ""
echo -e "${YELLOW}To install:${NC}"
echo -e "  sudo pacman -U ${PKG_FILE}"
echo ""
echo -e "${YELLOW}Public release alias:${NC}"
echo -e "  ${PROJECT_ROOT}/test/release_assets/${CB2000_RELEASE_SNAPSHOT:-R2.5}/public/arch_libfprint-canvasbio-cb2000.pkg.tar.zst"
echo ""
echo -e "${YELLOW}Includes:${NC}"
echo -e "  /usr/lib/libcb2000_sigfm_opencv.so        (OpenCV sidecar)"
echo -e "  /etc/udev/rules.d/99-canvasbio.rules      (USB permissions)"
echo -e "  /etc/polkit-1/rules.d/50-canvasbio-fprint.rules"
echo -e "${GREEN}========================================================${NC}"
