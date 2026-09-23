#!/usr/bin/env bash
# Runs the on-device check in a throwaway container, so nothing is installed
# on the machine you are testing from.
#
#   sudo tests/device/run.sh [--detect-only] [--verify N] [--impostor N]
#
# Root is needed because the reader's USB node belongs to root, which is also
# why fprintd runs as a system service.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
image="localhost/cb2000-selftest"

engine="$(command -v podman || command -v docker || true)"
if [ -z "$engine" ]; then
    echo "Needs podman or docker." >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this with sudo: the reader's device node belongs to root." >&2
    exit 1
fi

# The USB node, found by vendor and product so a replug is picked up.
node=""
for dev in /sys/bus/usb/devices/*; do
    [ -r "$dev/idVendor" ] || continue
    [ "$(cat "$dev/idVendor")" = "2df0" ] || continue
    [ "$(cat "$dev/idProduct")" = "0003" ] || continue
    node="$(printf '/dev/bus/usb/%03d/%03d' "$(cat "$dev/busnum")" "$(cat "$dev/devnum")")"
done
if [ -z "$node" ]; then
    echo "No 2df0:0003 reader found. Check 'lsusb | grep 2df0'." >&2
    exit 1
fi
echo "Reader at $node"

# fprintd claims the reader and holds it, and so do the login and lock
# screens, which start fprintd on their own.
if systemctl is-active --quiet fprintd 2>/dev/null; then
    echo "fprintd is running and holds the reader. Stop it first:" >&2
    echo "  sudo systemctl stop fprintd" >&2
    exit 1
fi

# Only what the build needs goes into the image: a working tree can hold
# anything, and a build context is copied whole.
ctx="$(mktemp -d)"
trap 'rm -rf "$ctx"' EXIT
cp -a "$repo/src" "$repo/tests" "$repo/meson.build" "$repo/meson_options.txt" "$ctx/"
cp "$repo/tests/device/Containerfile" "$ctx/"
# The version the check prints carries the commit, the same way a package
# built from the release tarball does (see meson.build).
if [ -e "$repo/.git" ]; then
    git -C "$repo" describe --always --dirty --exclude='*' > "$ctx/.commit"
elif [ -f "$repo/.commit" ]; then
    cp "$repo/.commit" "$ctx/.commit"
fi

"$engine" build -t "$image" -f "$ctx/Containerfile" "$ctx"

# When the terminal closes, the container would keep running and keep
# holding the reader (every later run then fails with "Resource busy"), so
# it is removed by name on the way out. The client runs in the background
# with the terminal passed in explicitly (a background job would get
# /dev/null), because bash runs a trap only after a foreground child
# exits, while "wait" returns as soon as a signal arrives.
name="cb2000-selftest-$$"
trap 'rm -rf "$ctx"; "$engine" rm -f "$name" >/dev/null 2>&1 || true' EXIT
trap 'exit 130' HUP INT TERM
"$engine" run --rm -it --init --name "$name" --device "$node" "$image" "$@" <&0 &
wait "$!"
