#!/usr/bin/env bash
# UNSUPPORTED build path.
#
# Builds the whole libfprint (pinned tag, all default drivers) with the
# CanvasBio CB2000 driver inside. `meson install` puts it under /usr/local;
# on Debian and Ubuntu that copy takes precedence over the distribution's
# libfprint and survives updates until you run `ninja -C <build> uninstall`.
# The supported path is the TOD module .deb; see README.md.
#
# Usage: packaging/in-tree/build.sh [work-dir]
# Needs git, meson, ninja, a C compiler and libfprint's own build
# dependencies (glib, gusb, pixman, openssl, gudev, udev, gobject-introspection).
set -euo pipefail

tag="v1.94.100"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
work="${1:-$PWD/libfprint-$tag-cb2000}"
patch="$repo/packaging/in-tree/libfprint-$tag.patch"

if [ -e "$work" ]; then
    echo "Refusing to reuse existing directory: $work" >&2
    exit 1
fi

git clone --depth 1 --branch "$tag" https://gitlab.freedesktop.org/libfprint/libfprint.git "$work"

driver_dir="$work/libfprint/drivers/canvasbio_cb2000"
mkdir -p "$driver_dir"
cp "$repo"/src/canvasbio_cb2000.c \
   "$repo"/src/cb2000_core.c \
   "$repo"/src/cb2000_core.h \
   "$repo"/src/cb2000_device.h \
   "$repo"/src/cb2000_engine.c \
   "$repo"/src/cb2000_engine.h \
   "$repo"/src/cb2000_engine_adapter.c \
   "$repo"/src/cb2000_engine_bytes.h \
   "$repo"/src/cb2000_engine_compare.c \
   "$repo"/src/cb2000_engine_enroll.c \
   "$repo"/src/cb2000_engine_minutiae.c \
   "$repo"/src/cb2000_engine_pet.c \
   "$repo"/src/cb2000_engine_score.c \
   "$repo"/src/cb2000_image.h \
   "$repo"/src/cb2000_print.c \
   "$repo"/src/cb2000_print.h \
   "$repo"/src/cb2000_protocol.c \
   "$repo"/src/cb2000_protocol.h \
   "$repo"/src/cb2000_tables.c \
   "$driver_dir/"

# Fails loudly if the patch does not apply to this tag.
git -C "$work" apply --check "$patch"
git -C "$work" apply "$patch"

version="$(sed -n "s/^ *version: '\([^']*\)',.*/\1/p" "$repo/meson.build" | head -n1)"
# Introspection stays on: at this tag, tests/meson.build fails to configure
# with -Dintrospection=false on current meson.
meson setup "$work/build" "$work" \
    -Ddoc=false \
    -Dgtk-examples=false \
    -Dinstalled-tests=false \
    "-Dc_args='-DCB2000_DRIVER_VERSION=\"$version+intree\"'"
meson compile -C "$work/build"

cat <<EOF

Built libfprint $tag with the CanvasBio CB2000 driver (UNSUPPORTED).
Installing it puts this libfprint under /usr/local, ahead of your
distribution's copy on Debian and Ubuntu:
    sudo meson install -C "$work/build"
Undo with:
    sudo ninja -C "$work/build" uninstall
EOF
