#!/usr/bin/env bash
# Maintainer script: prints the release version and refuses to continue if the
# packages disagree about it. The version lives in four files (meson.build,
# debian/changelog, the rpm spec, the PKGBUILD), checked before every build.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"

meson_version="$(sed -n "s/^ *version: '\([^']*\)',.*/\1/p" "$repo/meson.build" | head -n1)"
deb_version="$(sed -n '1s/.*(\([^)]*\)).*/\1/p' "$repo/packaging/debian/changelog")"
rpm_version="$(sed -n 's/^Version: *//p' "$repo/packaging/rpm/libfprint-2-tod1-canvasbio-cb2000.spec")"
arch_version="$(sed -n 's/^pkgver=//p' "$repo/packaging/arch/PKGBUILD")"

fail=0
for pair in "debian/changelog:$deb_version" "rpm spec:$rpm_version" "PKGBUILD:$arch_version"; do
    where="${pair%%:*}"
    what="${pair#*:}"
    if [ "$what" != "$meson_version" ]; then
        echo "Version mismatch: meson.build=$meson_version $where=$what" >&2
        fail=1
    fi
done
[ "$fail" = 0 ] || exit 1

echo "$meson_version"
