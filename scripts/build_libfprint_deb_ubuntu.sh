#!/usr/bin/env bash
# Build a tester-focused CanvasBio CB2000 .deb for Ubuntu / Debian.
#
# By default runs inside a persistent distrobox container (created on first run).
# Use --ephemeral for a throwaway container, --in-container if already inside.
#
# Usage:
#   ./scripts/build_libfprint_deb_ubuntu.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]
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
#   LIBFPRINT_DIR               Local libfprint source tree (default: $HOME/libfprint)
#   BUILD_DIR                   Working build directory (default: $HOME/libfprint-deb-build)
#   DBX_NAME                    Container name (default: cb2000-ubuntu)
#   CB2000_DEB_PACKAGE_NAME     Output Debian package name
#   CB2000_INCLUDE_POLKIT_RULE  Install permissive local fprint polkit rule (default: 1)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Container config ──────────────────────────────────────────────────────────
CONTAINER_IMAGE="${CONTAINER_IMAGE:-docker.io/library/ubuntu:24.04}"
DBX_NAME="${DBX_NAME:-cb2000-ubuntu}"

# ── Build paths ───────────────────────────────────────────────────────────────
LIBFPRINT_DIR="${LIBFPRINT_DIR:-${HOME}/libfprint}"
BUILD_DIR="${BUILD_DIR:-${HOME}/libfprint-deb-build}"
TIMESTAMP=$(date +%Y%m%d%H%M)
CUSTOM_VERSION="1.94.10+canvasbio.${TIMESTAMP}"
PACKAGE_NAME="${CB2000_DEB_PACKAGE_NAME:-libfprint-2-2-canvasbio}"
INCLUDE_POLKIT_RULE="${CB2000_INCLUDE_POLKIT_RULE:-1}"

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
echo -e "${YELLOW}>>> Checking Ubuntu/Debian environment...${NC}"

if ! grep -qiE "ubuntu|debian" /etc/os-release 2>/dev/null; then
    echo -e "${RED}ERROR: This script requires Ubuntu or Debian.${NC}"
    exit 1
fi

echo -e "${GREEN}>>> Installing build dependencies...${NC}"
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    dpkg-dev debhelper \
    meson ninja-build pkg-config \
    libglib2.0-dev libgusb-dev libpixman-1-dev \
    libnss3-dev libgudev-1.0-dev \
    gtk-doc-tools libgirepository1.0-dev \
    libcairo2-dev libusb-1.0-0-dev lsb-release git \
    libssl-dev libopencv-dev

echo -e "${YELLOW}>>> Building OpenCV sidecar...${NC}"
bash "${PROJECT_ROOT}/scripts/build_sigfm_opencv_helper.sh"

if [[ ! -d "${LIBFPRINT_DIR}" ]]; then
    echo -e "${YELLOW}>>> Cloning libfprint...${NC}"
    git clone https://gitlab.freedesktop.org/libfprint/libfprint.git "${LIBFPRINT_DIR}"
fi

echo -e "${YELLOW}>>> Syncing CanvasBio driver into libfprint tree...${NC}"
install_driver_into_tree "${LIBFPRINT_DIR}"

echo -e "${YELLOW}>>> Preparing build tree at ${BUILD_DIR}...${NC}"
rm -rf "${BUILD_DIR}"
cp -r "${LIBFPRINT_DIR}" "${BUILD_DIR}"
cd "${BUILD_DIR}"
install_driver_into_tree "${BUILD_DIR}"

echo -e "${YELLOW}>>> Building libfprint install tree...${NC}"
rm -rf builddir pkgroot
meson setup builddir --prefix=/usr -Ddoc=false -Dgtk-examples=false -Ddrivers="canvasbio_cb2000,virtual_image"
ninja -C builddir

STAGE_DIR="${BUILD_DIR}/pkgroot"
DESTDIR="${STAGE_DIR}" meson install -C builddir

ARCH="$(dpkg --print-architecture)"
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
DEBIAN_DIR="${STAGE_DIR}/DEBIAN"
DOC_DIR="${STAGE_DIR}/usr/share/doc/${PACKAGE_NAME}"
HELPER_DIR="${STAGE_DIR}/usr/lib/${MULTIARCH}"
UDEV_DIR="${STAGE_DIR}/etc/udev/rules.d"
POLKIT_DIR="${STAGE_DIR}/etc/polkit-1/rules.d"

mkdir -p "${DEBIAN_DIR}" "${DOC_DIR}" "${HELPER_DIR}" "${UDEV_DIR}"
install -m 0644 "${PROJECT_ROOT}/build/libcb2000_sigfm_opencv.so" "${HELPER_DIR}/libcb2000_sigfm_opencv.so"
install -m 0644 "${PROJECT_ROOT}/tools/99-canvasbio.rules" "${UDEV_DIR}/99-canvasbio.rules"
install -m 0644 "${PROJECT_ROOT}/README.md" "${DOC_DIR}/README.md"
install -m 0644 "${PROJECT_ROOT}/LICENSE" "${DOC_DIR}/LICENSE"

# Keep the release package runtime-only. Development headers, pkg-config files,
# GIR sources and installed-tests belong in private lab builds, not public assets.
rm -rf \
    "${STAGE_DIR}/usr/include" \
    "${STAGE_DIR}/usr/libexec/installed-tests" \
    "${STAGE_DIR}/usr/share/gir-1.0" \
    "${STAGE_DIR}/usr/share/installed-tests"
rm -f \
    "${HELPER_DIR}/libfprint-2.so" \
    "${HELPER_DIR}/pkgconfig/libfprint-2.pc"
rmdir --ignore-fail-on-non-empty "${HELPER_DIR}/pkgconfig" 2>/dev/null || true

if [[ "${INCLUDE_POLKIT_RULE}" == "1" ]]; then
    mkdir -p "${POLKIT_DIR}"
    cat > "${POLKIT_DIR}/50-canvasbio-fprint.rules" <<'POLKIT'
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("net.reactivated.fprint") === 0 && subject.local) {
        return polkit.Result.YES;
    }
});
POLKIT
fi

mkdir -p "${BUILD_DIR}/debian"
cat > "${BUILD_DIR}/debian/control" <<EOF
Source: ${PACKAGE_NAME}
Section: libs
Priority: optional
Maintainer: CanvasBio CB2000 Test Build <canvasbio@local>
Standards-Version: 4.7.0

Package: ${PACKAGE_NAME}
Architecture: ${ARCH}
Description: CanvasBio CB2000 tester build of libfprint with helper and host rules
 Runtime package used to compute shlib dependencies for the release asset build.
EOF

dpkg-shlibdeps \
    -T"${DEBIAN_DIR}/substvars" \
    -l"${HELPER_DIR}" \
    "${HELPER_DIR}/libfprint-2.so.2.0.0" \
    "${HELPER_DIR}/libcb2000_sigfm_opencv.so"

SHLIB_DEPS="$(sed -n 's/^shlibs:Depends=//p' "${DEBIAN_DIR}/substvars")"

cat > "${DEBIAN_DIR}/control" <<EOF
Package: ${PACKAGE_NAME}
Version: ${CUSTOM_VERSION}
Section: libs
Priority: optional
Architecture: ${ARCH}
Maintainer: CanvasBio CB2000 Test Build <canvasbio@local>
Depends: ${SHLIB_DEPS}
Provides: libfprint-2-2
Replaces: libfprint-2-2
Conflicts: libfprint-2-2
Description: CanvasBio CB2000 tester build of libfprint with helper and host rules
 This package contains a custom libfprint build with the CanvasBio CB2000
 driver enabled, the SIGFM OpenCV helper shared library, and the USB rule
 needed to lower friction for tester installs.
EOF

cat > "${DEBIAN_DIR}/postinst" <<'POSTINST'
#!/bin/sh
set -e
if command -v ldconfig >/dev/null 2>&1; then
    ldconfig || true
fi
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=usb --attr-match=idVendor=2df0 || true
fi
exit 0
POSTINST
chmod 755 "${DEBIAN_DIR}/postinst"

cat > "${DEBIAN_DIR}/postrm" <<'POSTRM'
#!/bin/sh
set -e
if command -v ldconfig >/dev/null 2>&1; then
    ldconfig || true
fi
exit 0
POSTRM
chmod 755 "${DEBIAN_DIR}/postrm"

echo -e "${GREEN}>>> Building .deb...${NC}"
DEB_FILE="${BUILD_DIR}/${PACKAGE_NAME}_${CUSTOM_VERSION}_${ARCH}.deb"
rm -f "${DEB_FILE}"
dpkg-deb --build --root-owner-group "${STAGE_DIR}" "${DEB_FILE}"

if [[ -z "${DEB_FILE}" ]]; then
    echo -e "${RED}ERROR: .deb not found after build.${NC}"
    exit 1
fi

echo -e "\n${GREEN}========================================================${NC}"
echo -e "${GREEN}SUCCESS: $(basename "${DEB_FILE}")${NC}"
echo -e "${GREEN}========================================================${NC}"
echo -e "Output: ${DEB_FILE}"
bash "${PROJECT_ROOT}/scripts/cb2000_prepare_release_asset.sh" \
    --distro ubuntu-debian \
    --package-name "${PACKAGE_NAME}" \
    --public-name libfprint-canvasbio-cb2000 \
    --artifact "${DEB_FILE}"
echo ""
echo -e "${YELLOW}To install:${NC}"
echo -e "  sudo dpkg -i ${DEB_FILE}"
echo -e "  sudo apt-get install -f   # fix dependencies if needed"
echo -e "  sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=2df0"
echo ""
echo -e "${YELLOW}Public release alias:${NC}"
echo -e "  ${PROJECT_ROOT}/test/release_assets/${CB2000_RELEASE_SNAPSHOT:-R2.5}/public/ubuntu-debian_libfprint-canvasbio-cb2000.deb"
echo -e "${GREEN}========================================================${NC}"
