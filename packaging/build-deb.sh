#!/usr/bin/env bash
# Maintainer script: builds the supported .deb inside the pinned container.
# End users install the released .deb; they never need to run this.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
image="localhost/cb2000-build:ubuntu-26.04"

version="$("$repo/packaging/version.sh")"
# shellcheck source=packaging/dist-report.sh
. "$repo/packaging/dist-report.sh"
echo "Building $version"

podman build -t "$image" -f "$repo/packaging/Containerfile" "$repo/packaging"

# The .deb is built from the release tarball, like the .rpm and the Arch
# package: same bytes, same .commit, and make-tarball.sh refuses a dirty tree.
tarball="canvasbio-cb2000-$version.tar.gz"
"$repo/packaging/make-tarball.sh" >/dev/null

mkdir -p "$repo/dist"
# Timestamps inside the package come from the commit, so building the same
# commit twice gives the same package.
podman run --rm \
    -v "$repo/dist/$tarball":/in/"$tarball":ro,Z \
    -v "$repo/dist":/out:Z \
    -e SOURCE_DATE_EPOCH="$(git -C "$repo" show -s --format=%ct HEAD)" \
    "$image" bash -euc "
        mkdir /tmp/build
        tar -xzf /in/$tarball -C /tmp/build
        cd /tmp/build/canvasbio-cb2000-$version
        cp -a packaging/debian debian
        dpkg-buildpackage -b -us -uc
        cp ../*.deb /out/
    "

dist_report "$repo" "$version" deb
