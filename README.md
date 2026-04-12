# canvasbio-cb2000

Linux `libfprint` driver for the CanvasBio CB2000 fingerprint reader.

This public repository is a functional `R2.5` snapshot. It documents the
current driver state, release packages and technical rationale for a working
but still development-shaped implementation. It is not yet a polished public
testing program or an upstream-ready submission.

## Status

- Driver: `src/canvasbio_cb2000.c`
- Matcher core: `src/cb2000_sigfm_matcher.c/.h`
- Helper sidecar: `src/cb2000_sigfm_opencv_helper.cpp`
- Sensor IDs: `0x2DF0:0x0003`, `0x2DF0:0x0007`
- Geometry: `80x64`, `340 dpi`, `6.0 mm x 4.8 mm`
- Enrollment flow: `15` stages
- Thermal model: virtual libfprint hot-shutdown disabled for this device
- Template model: `v2` mosaic path with fallback compatibility for older prints

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

## Installation Paths

Three realistic paths exist today.

### 1. Install a published package

This is the preferred path for anyone evaluating `R2.5` as a snapshot.

See `releases/README.md` for the package list and expected filenames.
The published runtime packages carry their runtime dependency metadata, so the
native package manager should pull the required OpenCV runtime pieces
automatically. Manual source builds still need the OpenCV development package.

### 2. Use the build/lab scripts

Use this if you want to reproduce the package builds or run the driver in the
current development-lab flow.

Entry points:

```bash
bash ./scripts/setup-libfprint.sh --all
bash ./scripts/build_sigfm_opencv_helper.sh
bash ./scripts/run_cb2000_interactive.sh
```

Ubuntu-oriented tester path:

```bash
bash ./scripts/cb2000_ubuntu_tester.sh prepare-host
bash ./scripts/cb2000_ubuntu_tester.sh prepare-runtime
bash ./scripts/cb2000_ubuntu_tester.sh test
bash ./scripts/cb2000_ubuntu_tester.sh build-deb
```

### 3. Integrate manually into libfprint

For full manual control, use the integration details in:

- `docs/CB2000_STANDALONE_PACKAGE_README.md`
- `scripts/README.md`

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
