#!/usr/bin/env bash
# Maintainer script: builds the release tarball that every package is built
# from, so the .deb, the two .rpm and the Arch package all come from the same
# bytes. Attach it to the GitHub release: the PKGBUILD downloads it by URL.
#
# The tarball is reproducible. Building it twice from the same commit gives
# the same sha256: git archive stamps every entry with the commit date and
# uid 0, the appended .commit file is stamped by hand, and gzip -n keeps no
# name or timestamp of its own.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
version="$("$repo/packaging/version.sh")"

commit="$(git -C "$repo" describe --always --dirty --exclude='*')"
case "$commit" in
    *-dirty)
        echo "Refusing to build a tarball from a dirty tree: commit first." >&2
        exit 1
        ;;
esac
epoch="$(git -C "$repo" show -s --format=%ct HEAD)"

name="canvasbio-cb2000-$version"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

git -C "$repo" archive --format=tar --prefix="$name/" HEAD -o "$tmp/src.tar"

# The driver reports its version with this commit appended; a build from a
# tarball has no checkout to ask (see meson.build).
printf '%s\n' "$commit" > "$tmp/.commit"
tar --append --file="$tmp/src.tar" \
    --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
    --transform="s,^,$name/," -C "$tmp" .commit

mkdir -p "$repo/dist"
gzip -9 -n -c "$tmp/src.tar" > "$repo/dist/$name.tar.gz"

cd "$repo/dist"
sha256sum "$name.tar.gz"
