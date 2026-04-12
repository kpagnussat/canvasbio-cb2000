#!/usr/bin/env bash
# Build a custom libfprint RPM for Fedora / Kinoite / Silverblue.
#
# By default runs inside a persistent distrobox container (created on first run).
# Use --ephemeral for a throwaway container, --in-container if already inside.
#
# Usage:
#   ./scripts/build_libfprint_rpm_fedora.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]
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
#   LIBFPRINT_DIR        Local libfprint source tree (default: $HOME/libfprint)
#   DBX_NAME             Container name (default: cb2000-fedora)
#   HOST_ISOLATED_HOME   Override host path in the rpm-ostree install tip
#                        (default: ~/.ContainerConfigs/$DBX_NAME)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Container config ──────────────────────────────────────────────────────────
CONTAINER_IMAGE="${CONTAINER_IMAGE:-registry.fedoraproject.org/fedora:43}"
DBX_NAME="${DBX_NAME:-cb2000-fedora}"

# Detect host-side config root (mirrors run_cb2000_interactive.sh logic)
if [[ "${HOME}" == */.ContainerConfigs/${DBX_NAME} ]]; then
    _CONFIG_ROOT="${HOME}"
elif [[ "${HOME}" == */.ContainerConfigs ]]; then
    _CONFIG_ROOT="${HOME}/${DBX_NAME}"
else
    _CONFIG_ROOT="${HOME}/.ContainerConfigs/${DBX_NAME}"
fi
HOST_ISOLATED_HOME="${HOST_ISOLATED_HOME:-${_CONFIG_ROOT}}"

# ── Build paths ───────────────────────────────────────────────────────────────
LIBFPRINT_DIR="${LIBFPRINT_DIR:-${HOME}/libfprint}"
RPMBUILD_DIR="${HOME}/rpmbuild"
TIMESTAMP=$(date +%Y%m%d%H%M)
RELEASE_TAG="99.canvasbio.${TIMESTAMP}"

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
    local env_fwd="LIBFPRINT_DIR='${LIBFPRINT_DIR}' DBX_NAME='${DBX_NAME}' HOST_ISOLATED_HOME='${HOST_ISOLATED_HOME}'"

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
                -e "DBX_NAME=${DBX_NAME}" \
                -e "HOST_ISOLATED_HOME=${HOST_ISOLATED_HOME}" \
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
echo -e "${YELLOW}>>> Checking Fedora/RHEL environment...${NC}"

if ! grep -qE "Fedora|CentOS|RHEL" /etc/os-release 2>/dev/null; then
    echo -e "${RED}ERROR: This script requires a Fedora/RHEL container.${NC}"
    exit 1
fi

echo -e "${GREEN}>>> Installing build tools...${NC}"
sudo dnf install -y fedpkg fedora-packager rpm-build opencv-devel gcc-c++ > /dev/null

mkdir -p "${RPMBUILD_DIR}"/{SOURCES,SPECS,RPMS,SRPMS,BUILD}

echo -e "${GREEN}>>> Fetching official Fedora spec...${NC}"
cd "${RPMBUILD_DIR}"
rm -rf libfprint-fedora
fedpkg clone -a libfprint libfprint-fedora
cd libfprint-fedora

echo -e "${GREEN}>>> Installing build dependencies...${NC}"
sudo dnf builddep -y libfprint.spec

echo -e "${YELLOW}>>> Packaging local source (${LIBFPRINT_DIR})...${NC}"
if [[ ! -d "${LIBFPRINT_DIR}" ]]; then
    echo -e "${RED}ERROR: ${LIBFPRINT_DIR} not found.${NC}"
    echo "Set LIBFPRINT_DIR or ensure a clone exists at \$HOME/libfprint"
    exit 1
fi

echo -e "${YELLOW}>>> Syncing CanvasBio driver into libfprint tree...${NC}"
install_driver_into_tree "${LIBFPRINT_DIR}"

SPEC_VERSION=$(grep "^Version:" libfprint.spec | awk '{print $2}')
TAR_NAME="libfprint-v${SPEC_VERSION}"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DIR}"' EXIT

cp -r "${LIBFPRINT_DIR}" "${TMP_DIR}/${TAR_NAME}"
rm -rf "${TMP_DIR}/${TAR_NAME}/.git"
install_driver_into_tree "${TMP_DIR}/${TAR_NAME}"

TARBALL_FILE="${RPMBUILD_DIR}/SOURCES/libfprint-v${SPEC_VERSION}.tar.bz2"
tar --directory="${TMP_DIR}" -cjf "${TARBALL_FILE}" "${TAR_NAME}"

echo -e "${YELLOW}>>> Building OpenCV sidecar...${NC}"
bash "${PROJECT_ROOT}/scripts/build_sigfm_opencv_helper.sh"
SIDECAR_SO="${PROJECT_ROOT}/build/libcb2000_sigfm_opencv.so"
cp "${SIDECAR_SO}" "${RPMBUILD_DIR}/SOURCES/"
install -m 0644 "${PROJECT_ROOT}/tools/99-canvasbio.rules" "${RPMBUILD_DIR}/SOURCES/99-canvasbio.rules"
cat > "${RPMBUILD_DIR}/SOURCES/50-canvasbio-fprint.rules" <<'POLKIT'
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("net.reactivated.fprint") === 0 && subject.local) {
        return polkit.Result.YES;
    }
});
POLKIT

echo -e "${YELLOW}>>> Patching .spec for custom release...${NC}"
cp libfprint.spec "${RPMBUILD_DIR}/SPECS/libfprint_custom.spec"
cd "${RPMBUILD_DIR}/SPECS"
sed -i "s/^Release:.*/Release:        ${RELEASE_TAG}%{?dist}/" libfprint_custom.spec
sed -i "s|^Source0:.*|Source0:        libfprint-v%{version}.tar.bz2|" libfprint_custom.spec
sed -i "s|^Source0:.*|&\nSource1:        libcb2000_sigfm_opencv.so\nSource2:        99-canvasbio.rules\nSource3:        50-canvasbio-fprint.rules|" libfprint_custom.spec
perl -0pi -e 's/^%install\n/%install\ninstall -Dm755 %{SOURCE1} %{buildroot}%{_libdir}\/libcb2000_sigfm_opencv.so\ninstall -Dm644 %{SOURCE2} %{buildroot}%{_sysconfdir}\/udev\/rules.d\/99-canvasbio.rules\ninstall -Dm644 %{SOURCE3} %{buildroot}%{_sysconfdir}\/polkit-1\/rules.d\/50-canvasbio-fprint.rules\nrm -rf %{buildroot}%{_includedir}\/libfprint-2\nrm -rf %{buildroot}%{_libexecdir}\/installed-tests\nrm -rf %{buildroot}%{_datadir}\/gir-1.0\nrm -rf %{buildroot}%{_datadir}\/installed-tests\nrm -f %{buildroot}%{_libdir}\/libfprint-2.so\nrm -f %{buildroot}%{_libdir}\/pkgconfig\/libfprint-2.pc\nrmdir %{buildroot}%{_libdir}\/pkgconfig 2>\/dev\/null || :\n/m' libfprint_custom.spec
sed -i "s|^%changelog|%post\nldconfig\n\n%changelog|" libfprint_custom.spec
sed -i "s|^%files$|%files\n%{_libdir}/libcb2000_sigfm_opencv.so\n%config(noreplace) %{_sysconfdir}/udev/rules.d/99-canvasbio.rules\n%config(noreplace) %{_sysconfdir}/polkit-1/rules.d/50-canvasbio-fprint.rules|" libfprint_custom.spec

echo -e "${GREEN}>>> Building RPM...${NC}"
rpmbuild -bb libfprint_custom.spec

RPM_PATH=$(find "${RPMBUILD_DIR}/RPMS" \
    -name "libfprint-${SPEC_VERSION}-${RELEASE_TAG}*.rpm" \
    | grep -v "debug" | grep -v "devel" | head -1)

if [[ -z "${RPM_PATH}" ]]; then
    echo -e "${RED}ERROR: built RPM not found.${NC}"
    exit 1
fi

RPM_NAME=$(basename "${RPM_PATH}")
RELATIVE_PATH="rpmbuild/RPMS/$(uname -m)/${RPM_NAME}"

echo -e "\n${GREEN}========================================================${NC}"
echo -e "${GREEN}SUCCESS: ${RPM_NAME}${NC}"
echo -e "${GREEN}========================================================${NC}"
echo -e "Output (inside container): ~/${RELATIVE_PATH}"
bash "${PROJECT_ROOT}/scripts/cb2000_prepare_release_asset.sh" \
    --distro fedora \
    --package-name libfprint \
    --public-name libfprint-canvasbio-cb2000 \
    --artifact "${RPM_PATH}"
echo ""
echo -e "${YELLOW}Kinoite / Silverblue — run on HOST:${NC}"
echo -e "  sudo rpm-ostree override replace ${HOST_ISOLATED_HOME}/${RELATIVE_PATH}"
echo ""
echo -e "${YELLOW}Public release alias:${NC}"
echo -e "  ${PROJECT_ROOT}/test/release_assets/${CB2000_RELEASE_SNAPSHOT:-R2.5}/public/fedora_libfprint-canvasbio-cb2000.rpm"
echo ""
echo -e "${YELLOW}Includes:${NC}"
echo -e "  %{_libdir}/libcb2000_sigfm_opencv.so      (OpenCV sidecar)"
echo -e "  /etc/udev/rules.d/99-canvasbio.rules      (USB permissions)"
echo -e "  /etc/polkit-1/rules.d/50-canvasbio-fprint.rules"
echo -e "${GREEN}========================================================${NC}"
