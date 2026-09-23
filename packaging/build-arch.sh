#!/usr/bin/env bash
# Maintainer script: writes the PKGBUILD that goes on the release page, which
# is the in-tree one with the checksum of the release tarball filled in.
#
# The checksum cannot live in the tree: the tarball is made from the tree, so
# writing the sum into the PKGBUILD would change the tarball it describes.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
version="$("$repo/packaging/version.sh")"

"$repo/packaging/make-tarball.sh" >/dev/null
sum="$(sha256sum "$repo/dist/canvasbio-cb2000-$version.tar.gz" | cut -d' ' -f1)"

sed "s/^sha256sums=.*/sha256sums=('$sum')/" \
    "$repo/packaging/arch/PKGBUILD" > "$repo/dist/PKGBUILD"

grep -E '^(pkgver|sha256sums)=' "$repo/dist/PKGBUILD"
echo "dist/PKGBUILD"
