#!/usr/bin/env bash
# Maintainer script: builds the .rpm inside a container of the target
# distribution, against that distribution's own libfprint-tod headers.
#
#   packaging/build-rpm.sh opensuse    # Tumbleweed, TOD comes from the OSS repo
#   packaging/build-rpm.sh fedora      # TOD comes from a third party COPR
#
# End users install the released .rpm; they never need to run this.
set -euo pipefail

target="${1:-}"
case "$target" in
    fedora|opensuse) ;;
    *) echo "usage: $0 fedora|opensuse" >&2; exit 2 ;;
esac

repo="$(cd "$(dirname "$0")/.." && pwd)"
version="$("$repo/packaging/version.sh")"
# shellcheck source=packaging/dist-report.sh
. "$repo/packaging/dist-report.sh"
spec="packaging/rpm/libfprint-2-tod1-canvasbio-cb2000.spec"
tarball="canvasbio-cb2000-$version.tar.gz"

"$repo/packaging/make-tarball.sh" >/dev/null

if [ "$target" = fedora ]; then
    image="registry.fedoraproject.org/fedora:44"
    # Fedora sets %dist itself (.fc44); openSUSE leaves it empty, and a
    # release page with two files called the same thing helps nobody.
    dist=""
    # The COPR that ships both the TOD library and the SELinux policy the
    # package requires, see the spec.
    setup='dnf -y install dnf-plugins-core >/dev/null
           dnf -y copr enable ferdiu/libfprint-tod >/dev/null
           dnf -y install rpm-build rpmdevtools gcc meson ninja-build \
               pkgconf-pkg-config glib2-devel libgusb-devel \
               libfprint-tod-devel >/dev/null'
else
    image="registry.opensuse.org/opensuse/tumbleweed:latest"
    dist=".opensuse"
    setup='zypper --non-interactive --gpg-auto-import-keys refresh >/dev/null
           zypper --non-interactive install -y rpm-build rpmdevtools gcc meson \
               ninja pkg-config glib2-devel libgusb-devel \
               libfprint-tod-devel >/dev/null'
fi

mkdir -p "$repo/dist"
podman run --rm \
    -v "$repo/dist/$tarball":/in/"$tarball":ro,Z \
    -v "$repo/$spec":/in/pkg.spec:ro,Z \
    -v "$repo/dist":/out:Z \
    -e SOURCE_DATE_EPOCH="$(git -C "$repo" show -s --format=%ct HEAD)" \
    "$image" bash -euc "
        $setup
        rpmdev-setuptree
        cp /in/$tarball ~/rpmbuild/SOURCES/
        cp /in/pkg.spec ~/rpmbuild/SPECS/
        [ -z '$dist' ] || echo '%dist $dist' >> ~/.rpmmacros
        rpmbuild -bb ~/rpmbuild/SPECS/pkg.spec
        find ~/rpmbuild/RPMS -name '*.rpm' ! -name '*debug*' -exec cp {} /out/ \;
    "

dist_report "$repo" "$version" rpm
