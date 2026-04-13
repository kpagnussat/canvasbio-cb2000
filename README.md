# canvasbio-cb2000

Linux `libfprint` driver for the CanvasBio CB2000 fingerprint reader.

This public repository is a functional `R2.5` snapshot. It documents the
current driver state, release packages and technical rationale for a working
but still development-shaped implementation. It is not yet a polished public
testing program or an upstream-ready submission.

Public snapshot assumptions for `R2.5`:

- OpenCV is still required in the active runtime/build path
- enrollment is intentionally `15` stages
- libfprint virtual thermal hot-shutdown is explicitly disabled for this device
- this repository is a functional technical snapshot, not the final reduced-dependency public phase

## Status

- Driver: `src/canvasbio_cb2000.c`
- Matcher core: `src/cb2000_sigfm_matcher.c/.h`
- Helper sidecar: `src/cb2000_sigfm_opencv_helper.cpp`
- Sensor IDs: `0x2DF0:0x0003`, `0x2DF0:0x0007`
- Geometry: `80x64`, `340 dpi`, `6.0 mm x 4.8 mm`
- Enrollment flow: `15` stages
- Thermal model: virtual libfprint hot-shutdown disabled for this device
- Template model: multi-capture feature mosaic with SIGFM-based matching

Current controlled-test snapshot:

| Metric | Result |
|--------|--------|
| GAR | `100%` |
| FAR | `0%` in controlled wrong-finger tests |
| Enrollment stages | `15` |
| Template format | `v2` |

These results come from controlled technical validation, not from a broad
multi-user public study.

## Audience Guide

- Want packages to install:
  see `releases/README.md`
- Want the technical rationale:
  see `docs/DRIVER_NOTES.md`
- Want implementation details, findings and architecture notes:
  see `docs/`
- Want build and lab tooling:
  see `docs/DEVELOPER_LAB.md` and `scripts/README.md`

## Requirements

| Component | Requirement |
|-----------|-------------|
| Compiler | `gcc`, `g++` |
| Build system | `meson`, `ninja-build` |
| OpenCV | 4.x (`libopencv-dev` or distro equivalent), required in the current `R2.5` path |
| libfprint | source tree, `>= 1.94` |
| Python | `>= 3.10` for analysis/report helpers |

The current `R2.5` snapshot depends on an OpenCV sidecar for the active
feature-mosaic path. In other words: for the published `R2.5` code and
packages, OpenCV is not optional yet.

## Why 15 Enrollment Stages

The CB2000 captures only a small local region of the finger on each press. A
small number of enrollment samples tends to produce:

- weak spatial coverage
- repeated local regions
- poor descriptor diversity
- more `enroll-retry-scan`
- more `enroll-remove-and-retry`
- more false `no-match` events later

So the current driver uses a `15`-stage lift-and-shift enrollment flow. The
goal is not "15 photos" as a cosmetic number. The goal is enough distinct local
coverage to build a stronger feature mosaic from a very small sensor.

In controlled tuning, shorter flows under-covered the finger area. The retained
`15`-stage path consistently produced the strongest mosaic quality and the
lowest later `retry` / `no-match` friction for this sensor class.

Technical rationale:
- `docs/DRIVER_NOTES.md`
- `docs/DECISIONS.md`
- `docs/FINDINGS.md`

## Why OpenCV Is Still Present

This snapshot still uses an OpenCV sidecar because the active template path is
feature-mosaic based.

Current division of work:

- `src/canvasbio_cb2000.c`
  main libfprint driver
- `src/cb2000_sigfm_matcher.c`
  in-driver SIGFM matcher core in C
- `src/cb2000_sigfm_opencv_helper.cpp`
  OpenCV helper for feature extraction, alignment and mosaic support

The longer-term direction is to remove or replace this dependency, but that work
has not been completed in `R2.5`. Public `R2.5` packages should therefore be
understood as OpenCV-dependent runtime packages.

## Why Thermal Override Is Enabled

The driver sets:

```c
dev_class->temp_hot_seconds = -1; /* Solves false temperature hot shutdown */
```

That disables libfprint's generic virtual thermal throttling model for this
device. In practice, the generic model was causing false `FP_TEMPERATURE_HOT`
shutdowns during valid enrollment sessions, especially with the longer
multi-sample collection required by the CB2000.

## Licensing

The repository contains a mix of project-authored code and clearly identified
third-party-derived portions.

- Project-authored source files are distributed under `LGPL-2.1-or-later`
- adapted SIGFM portions retain the required upstream MIT notice
- OpenCV is a current external dependency in `R2.5`, not vendored source in this repo

See:

- `LICENSE`
- `COPYRIGHT`
- `THIRD_PARTY_NOTICES.md`
- `third_party_licenses/SIGFM-MIT.txt`

## Installation Paths

Three realistic paths exist today.

### 1. Install a published package

This is the preferred path for anyone evaluating `R2.5` as a snapshot.

See `releases/README.md` for the package list and expected filenames.
The published runtime packages carry their runtime dependency metadata, so the
native package manager should pull the required OpenCV runtime pieces
automatically. Manual source builds still need the OpenCV development package.

### 2. Use the build/lab scripts (with distrobox)

Use this if you want to reproduce the package builds or run the driver in the
current development-lab flow. Requires `distrobox` on the host.

```bash
# one-time: create the Ubuntu container and build everything inside it
bash ./scripts/cb2000_ubuntu_tester.sh prepare-host     # install udev rule on host (needs sudo)
bash ./scripts/cb2000_ubuntu_tester.sh prepare-runtime  # install deps, clone + build libfprint inside container

# interactive enroll + verify session (CB2000 device required)
bash ./scripts/cb2000_ubuntu_tester.sh interactive

# or build a distributable .deb
bash ./scripts/cb2000_ubuntu_tester.sh build-deb
```

Alternatively, enter the container and drive the scripts directly:

```bash
distrobox enter canvasbio -- bash -lc "
  cd /path/to/canvasbio-cb2000
  bash scripts/setup/setup-libfprint.sh --install-deps --clone --integrate --build
  bash scripts/packaging/build_sigfm_opencv_helper.sh
  bash scripts/run_cb2000_interactive.sh --in-container
"
```

### 3. Build on the host (no distrobox)

Use this if you prefer a direct host build without any container tooling.

#### 3a. Install build dependencies

Ubuntu / Debian:

```bash
sudo apt install build-essential pkg-config meson ninja-build \
  libglib2.0-dev libgusb-dev libpixman-1-dev libnss3-dev \
  libgudev-1.0-dev gtk-doc-tools libgirepository1.0-dev \
  libcairo2-dev libusb-1.0-0-dev libssl-dev libopencv-dev \
  python3-venv git
```

Fedora:

```bash
sudo dnf install gcc gcc-c++ meson ninja-build pkg-config \
  glib2-devel libgudev-devel libgusb-devel pixman-devel \
  nss-devel gtk-doc gobject-introspection-devel \
  cairo-devel libusb1-devel openssl-devel opencv-devel \
  python3 git
```

Arch Linux:

```bash
sudo pacman -S --needed base-devel meson ninja pkg-config \
  glib2 libgudev libgusb pixman nss gtk-doc gobject-introspection \
  cairo libusb openssl opencv python python-pip git
```

openSUSE Tumbleweed:

```bash
sudo zypper install gcc gcc-c++ meson ninja pkg-config \
  glib2-devel libgudev-1_0-devel libgusb-devel pixman-devel \
  mozilla-nss-devel gtk-doc gobject-introspection-devel \
  cairo-devel libusb-1_0-devel libopenssl-devel opencv-devel \
  python3 git
```

#### 3b. Integrate and build libfprint

The integration script works directly on the host; no container needed:

```bash
# Clone libfprint and integrate the CB2000 driver into it
LIBFPRINT_DIR="$HOME/libfprint" \
  bash scripts/setup/setup-libfprint.sh --clone --integrate --build
```

This clones libfprint into `$LIBFPRINT_DIR`, copies the driver sources under
`libfprint/libfprint/drivers/canvasbio_cb2000/`, patches `meson.build`, and
runs the meson + ninja build.

If you prefer to do it manually without the script:

```bash
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git ~/libfprint

# copy driver files
mkdir -p ~/libfprint/libfprint/drivers/canvasbio_cb2000
cp src/canvasbio_cb2000.c        ~/libfprint/libfprint/drivers/canvasbio_cb2000/
cp src/cb2000_sigfm_matcher.c    ~/libfprint/libfprint/drivers/canvasbio_cb2000/
cp src/cb2000_sigfm_matcher.h    ~/libfprint/libfprint/drivers/canvasbio_cb2000/
cp src/meson.build               ~/libfprint/libfprint/drivers/canvasbio_cb2000/

# add the driver to libfprint/libfprint/meson.build
# find the driver_sources dict and add (after 'focaltech_moc'):
#   'canvasbio_cb2000' :
#       [ 'drivers/canvasbio_cb2000/canvasbio_cb2000.c',
#         'drivers/canvasbio_cb2000/cb2000_sigfm_matcher.c' ],

# build
cd ~/libfprint
meson setup builddir -Ddoc=false -Dgtk-examples=false
ninja -C builddir
```

#### 3c. Build the OpenCV sidecar

```bash
bash scripts/packaging/build_sigfm_opencv_helper.sh
# output: build/libcb2000_sigfm_opencv.so
```

This must be done from the project root. The sidecar is loaded at runtime via
`LD_LIBRARY_PATH`; it does not get installed system-wide.

#### 3d. Install the udev rule

```bash
sudo cp tools/99-canvasbio.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Or use the helper script (also handles the case where the rule is already
installed):

```bash
sudo bash scripts/setup/cb2000_usb_host_access.sh --ensure
```

#### 3e. Run

```bash
# enroll (15 stages)
LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ~/libfprint/builddir/examples/enroll

# verify
LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ~/libfprint/builddir/examples/verify
```

`CB2000_OUTPUT_DIR` can be set to a writable path to capture session images and
logs in a specific directory.

## Package Outputs

The current distro package families are:

- Ubuntu/Debian: `.deb`
- Fedora/Kinoite/Silverblue: `.rpm`
- Arch Linux: `.pkg.tar.zst`
- openSUSE Tumbleweed/Aeon/MicroOS: `.rpm`

Only the runtime/install packages should be attached to public releases. Do not
attach debug, debugsource, tests or devel artifacts unless there is a specific
technical reason.

Package names are intentionally distro-native rather than globally identical.
That keeps each package aligned with the host package manager's dependency and
replacement model while still publishing exactly one runtime artifact per distro
family in public releases.

For GitHub Releases, the release workflow stages normalized attachment aliases
in the form `<distro>_libfprint-canvasbio-cb2000.<extension>` so the upload set
stays stable even when distro-native build filenames include changing version
or release fields.

## Repository Layout

- `src/`
  active driver, matcher, helper source and Meson integration
- `scripts/`
  build, runtime, analysis and packaging helpers
- `docs/`
  technical rationale, findings, architecture notes and package walkthroughs
- `tools/`
  auxiliary assets such as `udev` rules
- `releases/`
  package and release guidance

## Support Status

This repository is published as a working snapshot that functions with its
current dependency set. It is not yet presented as:

- a broad public testing campaign
- a dependency-reduced final packaging shape
- an upstream-ready libfprint submission

Those phases still require more work, especially around dependency reduction and
matcher/helper refactoring.
