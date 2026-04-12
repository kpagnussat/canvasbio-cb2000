# CanvasMancer

Linux `libfprint` driver for the CanvasBio CB2000 fingerprint reader.

This repository serves two public audiences:

- users and testers who want the current driver state and installable packages
- developers and tinkerers who want to inspect, rebuild, debug, and improve the driver

It does not expose internal project workflow that is too personal or too specific
to the local development environment.

## Current State

- Active driver: `src/canvasbio_cb2000.c`
- Active matcher core: `src/cb2000_sigfm_matcher.c/.h`
- Active helper sidecar: `build/libcb2000_sigfm_opencv.so` built from repo sources
- Sensor IDs: `0x2DF0:0x0003` and `0x2DF0:0x0007`
- Geometry: `80x64`, `340 dpi`, `6.0 mm x 4.8 mm`
- Enrollment flow: `15` stages
- Thermal model: libfprint virtual hot-shutdown disabled for this device

The driver is functional, but still under active development and tuning. This
public repository should be treated as a working `R2.5` snapshot, not as a
fully supported end-user product line.

## Audience Guide

- Want packages to install:
  see `releases/README.md`
- Want a high-level driver explanation:
  see `docs/DRIVER_NOTES.md`
- Want the development lab and helper scripts:
  see `docs/DEVELOPER_LAB.md` and `scripts/README.md`
- Want deeper technical references:
  see `docs/`

## Why 15 Enrollment Stages

The CB2000 captures a very small region of the finger on each press. Each
accepted image is only one local view, not a full fingerprint.

Using just a few samples increases the chance of:

- weak spatial coverage
- duplicated local regions
- poor mosaic diversity
- `enroll-retry-scan`
- `enroll-remove-and-retry`
- later `no-match` against the correct finger

The current `15`-stage flow exists to collect enough distinct local coverage to
build a more robust feature mosaic. In practice, the user should do a
lift-and-shift motion with small positional changes rather than repeatedly
pressing exactly the same region.

## Why Thermal Override Is Enabled

The driver sets:

```c
dev_class->temp_hot_seconds = -1; /* Solves false temperature hot shutdown */
```

This disables libfprint's generic virtual thermal throttling model for this
device. In real use, that model was causing false `FP_TEMPERATURE_HOT` shutdowns
during enrollment, especially because the CB2000 workflow may legitimately spend
longer collecting many small coverage samples.

This is not a claim that thermal behavior is irrelevant in every context. It is
a pragmatic compatibility choice to avoid a false software-side stop condition
that was blocking valid enroll flows on this hardware.

## Repository Layout

- `src/`
  active driver, matcher, helper source and Meson integration
- `scripts/`
  development lab helpers, integration helpers, runtime flows and package builders
- `docs/`
  driver notes, lab notes, findings and durable references
- `tools/`
  auxiliary files such as `udev` rules
- `releases/`
  release-facing notes and pointers to installable packages

## Packages

Installable distro packages should be published through project releases. The
repository now reserves `releases/` as the canonical place to describe package
formats, expected file names and installation notes.

Current package families:

- Ubuntu/Debian: `.deb`
- Fedora/Kinoite/Silverblue: `.rpm`
- Arch Linux: `.pkg.tar.zst`
- openSUSE Tumbleweed/Aeon/MicroOS: `.rpm`

See `releases/README.md` for details.

## Development Lab

The scripts in `scripts/` are intentionally kept for developers, contributors
and tinkerers. They are not presented as a polished end-user installer.

That lab exists to help with:

- local integration into `libfprint`
- USB permission preparation
- containerized runtime testing
- artifact collection
- report generation
- distro package generation

See `docs/DEVELOPER_LAB.md` and `scripts/README.md`.

## Support Status

This public repository is a functional snapshot meant to document the current
driver state and provide reproducible build/test paths. It is not yet presented
as a supported public testing program. Dependency reduction and further cleanup
are still planned before broader public testing phases.
