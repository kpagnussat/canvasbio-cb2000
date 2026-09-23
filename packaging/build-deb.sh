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

mkdir -p "$repo/dist"
# Timestamps inside the package come from the commit, so building the same
# commit twice gives the same package.
podman run --rm \
    -v "$repo":/src:ro,Z \
    -v "$repo/dist":/out:Z \
    -e SOURCE_DATE_EPOCH="$(git -C "$repo" show -s --format=%ct HEAD)" \
    "$image" bash -euc '
        git config --global --add safe.directory "*"
        cp -a /src /tmp/build
        cd /tmp/build
        rm -rf debian build*
        cp -a packaging/debian debian
        dpkg-buildpackage -b -us -uc
        cp ../*.deb /out/
    '

dist_report "$repo" "$version" deb
