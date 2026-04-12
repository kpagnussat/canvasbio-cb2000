# Scripts Documentation

This directory contains the development-lab helpers for the
`canvasbio-cb2000` driver package.

The active lab assumes a dedicated per-container root under
`~/.ContainerConfigs/<DBX_NAME>/`. In the default container flow:

- `setup-libfprint.sh` uses `~/.ContainerConfigs/<DBX_NAME>/libfprint`
- runtime artifacts go under `~/.ContainerConfigs/<DBX_NAME>/cb2000_runtime`
- distro package builders emit host-facing install tips against that isolated root

These scripts are public on purpose, but they should be read as developer and
tinkerer tooling, not as a polished end-user installer.

The scripts fall into four groups:

- **Orchestration**: the interactive entry point for day-to-day development
- **Integration/build**: prepare libfprint, compile the sidecar helper, rebuild
  the driver
- **Session/runtime**: run enroll/verify sessions and collect artifacts
- **Analysis/reporting**: inspect session logs and summarize gate behavior

## Recommended Workflows

### Normal development flow

```text
1. cb2000_orchestrator.sh    -> configure environment (once)
2. cb2000_orchestrator.sh    -> option [2]: create Python venv (once)
3. cb2000_orchestrator.sh    -> option [3]: build OpenCV helper (.so)
4. cb2000_orchestrator.sh    -> option [4]: build libfprint driver
5. cb2000_orchestrator.sh    -> option [5]: full session (enroll + verify)
```

For daily use after initial setup:

```text
cb2000_orchestrator.sh -> option [6]: session without rebuild
```

### Release/integration flow

```text
1. setup-libfprint.sh --check
2. setup-libfprint.sh --install-deps --clone --integrate --build
3. build_sigfm_opencv_helper.sh
4. run_cb2000_test.sh or run_cb2000_interactive.sh
```

`setup-libfprint.sh` is for preparing a local libfprint tree. The OpenCV helper
remains a sidecar built from this repo, and the default target tree is the
isolated lab path under `~/.ContainerConfigs/<DBX_NAME>/libfprint`.

### Low-friction Ubuntu tester flow

```text
1. cb2000_ubuntu_tester.sh prepare-host
2. cb2000_ubuntu_tester.sh prepare-runtime
3. cb2000_ubuntu_tester.sh test        or interactive
4. cb2000_ubuntu_tester.sh build-deb
```

This path is for non-developer testers, but it still belongs to the development
lab. It installs the host USB rule, prepares the dedicated Ubuntu container,
validates the driver build, and can emit a tester-focused `.deb`. The default
container name here is `canvasbio`, and the runtime/build roots stay under
`~/.ContainerConfigs/canvasbio/`.

### Packaging flow (distro packages)

Each distro has a dedicated hardened build script. All scripts share the same
container management pattern: they auto-create a named distrobox container on
first run, or accept `--ephemeral` for a throwaway container.

| Distro | Script | Container image | Package format |
|--------|--------|-----------------|----------------|
| Fedora / Kinoite / Silverblue | `build_libfprint_rpm_fedora.sh` | `fedora:43` | `.rpm` via `fedpkg` + `rpmbuild` |
| Ubuntu / Debian | `build_libfprint_deb_ubuntu.sh` | `ubuntu:24.04` | `.deb` via staged install tree + `dpkg-deb` |
| Arch Linux | `build_libfprint_pkg_arch.sh` | `archlinux:latest` | `.pkg.tar.zst` via `makepkg` |
| openSUSE Tumbleweed / Aeon / MicroOS | `build_libfprint_rpm_opensuse.sh` | `opensuse/tumbleweed:latest` | `.rpm` via `rpmbuild` |

All scripts support five operating modes:

```text
(no flag)            Auto: enter persistent container, create it if needed
--in-container       Already inside the correct container — run build directly
--no-container       Run directly on the host when that distro/toolchain is already available
--ephemeral (-e)     Throwaway container — destroyed after build completes
--persistent (-p)    Use/create a named persistent distrobox container (default)
--create-container   Create the container only, skip the build
```

Host-path note:

- Fedora/openSUSE builders print the final host install command with the
  package path already resolved against `HOST_ISOLATED_HOME`
- by default `HOST_ISOLATED_HOME` resolves to `~/.ContainerConfigs/<DBX_NAME>`
- this avoids the old ambiguity between the in-container path and the host path

Quick examples:

```bash
# Fedora/Kinoite — auto (creates cb2000-fedora container on first run)
./scripts/build_libfprint_rpm_fedora.sh

# Ubuntu — throwaway container, custom source path
LIBFPRINT_DIR="$HOME/libfprint" ./scripts/build_libfprint_deb_ubuntu.sh --ephemeral

# Arch — just create the container, build later
./scripts/build_libfprint_pkg_arch.sh --create-container

# openSUSE — custom container name
DBX_NAME=my-suse ./scripts/build_libfprint_rpm_opensuse.sh
```

On atomic desktops (Kinoite/Silverblue/Aeon/MicroOS) each script prints
the exact `rpm-ostree override replace` or `transactional-update pkg install`
command to run on the host after the build completes.

## Script Reference

### `cb2000_orchestrator.sh` — Interactive Main Menu

The primary entry point. It manages environment setup, Python venv, helper
build, driver rebuild, and interactive test sessions.

It saves configuration to `.cb2000_env` (gitignored), so you do not need to
re-enter the same paths every run.

```text
Usage: ./scripts/cb2000_orchestrator.sh

Menu options:
  [1] Configure environment (build target, container name, libfprint path, log root)
  [2] Create / update Python venv
  [3] Build OpenCV helper (.so)
  [4] Build libfprint driver
  [5] Full session: build + enroll + verify (interactive)
  [6] Session without build: enroll + verify (interactive)
  [7] Battery report (GAR/FAR for latest session)
  [Q] Quit
```

### `setup-libfprint.sh` — Integrate Into a Local libfprint Tree

Copies the CB2000 driver sources into a local libfprint checkout, adds the
driver directory to the libfprint tree when needed, and can trigger a rebuild.

This is the quickest way to prepare a clean local integration for release
verification.

```bash
Usage: ./scripts/setup-libfprint.sh [options]

Options:
  --install-deps    Install build dependencies (Ubuntu/Debian)
  --clone           Clone or update libfprint
  --integrate       Copy driver files into libfprint/libfprint/drivers/canvasbio_cb2000
  --build           Build libfprint with canvasbio_cb2000 enabled
  --check           Syntax-check local driver source files
  --all             Run install-deps + clone + integrate + build
```

Notes:

- It integrates the main C driver sources only.
- The feature-mosaic OpenCV helper is built separately by
  `build_sigfm_opencv_helper.sh`.
- The optional udev rule lives in `tools/99-canvasbio.rules`.

### `build_sigfm_opencv_helper.sh` — Build OpenCV Helper

Compiles `cb2000_sigfm_opencv_helper.cpp` into
`build/libcb2000_sigfm_opencv.so`.

```bash
Usage: bash scripts/build_sigfm_opencv_helper.sh

Environment variables:
  CB2000_OPENCV_PKG       pkg-config name for OpenCV (default: auto-detect)
```

### `run_cb2000_interactive.sh` — Interactive Enrollment + Verify Session

Runs a full enrollment + verify session, optionally inside a distrobox
container. It copies the current driver sources to the libfprint tree, rebuilds
when necessary, and launches the test binary.

```bash
Usage: ./scripts/run_cb2000_interactive.sh [--in-container | --no-container]

Environment variables:
  CB2000_LOG_ROOT          Log output directory (default: ../dev_logs/sessions)
  CB2000_SESSION_ID        Override session timestamp ID
  CB2000_LIBFPRINT_ROOT    Override libfprint root path
  DBX_NAME                 Container name (default: canvasbio)
  CB2000_USE_SUDO          Set to 1 to run test binary with sudo
```

### `run_cb2000_test.sh` — Non-Interactive Test Run

Runs the test binary in a direct test path. Useful for quick regression checks
and controlled release sanity passes.

The current driver behavior is the active path: enrollment is **15 stages**.
Any old reference to `5 scans` belongs to an obsolete prototype flow.

Those `15` stages are intentional. Each accepted image covers only a small local
region of the finger, so the driver needs enough distinct lift-and-shift samples
to build a more robust mosaic and reduce later `retry` or `no-match` behavior.

```bash
Usage: ./scripts/run_cb2000_test.sh [--in-container | --no-container]
```

The runner now performs a USB-access preflight before touching the sensor. If
the CB2000 is visible but not writable, it points to
`cb2000_usb_host_access.sh --ensure` instead of failing late inside libusb.

### `cb2000_usb_host_access.sh` — Host USB Rule Helper

Checks whether the CB2000 device node is writable and, when needed, installs
`tools/99-canvasbio.rules` on the host before reloading `udev`.

```bash
Usage: ./scripts/cb2000_usb_host_access.sh [--check | --install-host-rule | --ensure]
```

### `cb2000_ubuntu_tester.sh` — Ubuntu Tester Entry Point

Single entry point for low-friction tester workflows on Ubuntu-based setups.

```bash
Usage: ./scripts/cb2000_ubuntu_tester.sh [prepare-host | prepare-runtime | test | interactive | build-deb | all]
```

### `build_libfprint_rpm_fedora.sh` — Build RPM for Fedora / Kinoite / Silverblue

Builds a custom libfprint RPM inside a Fedora 43 distrobox container.
Uses `fedpkg` to pull the official `.spec`, injects the local source tree as a
custom tarball, and bumps the release tag to `99.canvasbio.<timestamp>` so it
supersedes the distro package.

On atomic desktops (Kinoite/Silverblue) the output tip provides the full
`rpm-ostree override replace` command to run on the host.

```bash
Usage: ./scripts/build_libfprint_rpm_fedora.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]

Environment variables:
  LIBFPRINT_DIR        Source tree to package (default: $HOME/libfprint)
  DBX_NAME             Container name (default: cb2000-fedora)
  HOST_ISOLATED_HOME   Override host path in the rpm-ostree install tip
  CONTAINER_IMAGE      Override container image (default: registry.fedoraproject.org/fedora:43)
```

### `build_libfprint_deb_ubuntu.sh` — Build .deb for Ubuntu / Debian

Builds a custom libfprint `.deb` inside an Ubuntu 24.04 distrobox container
using `dpkg-buildpackage`.

```bash
Usage: ./scripts/build_libfprint_deb_ubuntu.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]

Environment variables:
  LIBFPRINT_DIR    Source tree to package (default: ~/.ContainerConfigs/<DBX_NAME>/libfprint when using the isolated lab)
  BUILD_DIR        Working build directory (default: $HOME/libfprint-deb-build)
  DBX_NAME         Container name (default: cb2000-ubuntu)
  CONTAINER_IMAGE  Override container image (default: docker.io/library/ubuntu:24.04)
```

### `build_libfprint_pkg_arch.sh` — Build package for Arch Linux

Generates a `PKGBUILD` from the local source tree and builds a
`.pkg.tar.zst` with `makepkg`. Runs inside an Arch Linux distrobox container.

```bash
Usage: ./scripts/build_libfprint_pkg_arch.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]

Environment variables:
  LIBFPRINT_DIR    Source tree to package (default: $HOME/libfprint)
  BUILD_DIR        Working build directory (default: $HOME/libfprint-arch-build)
  DBX_NAME         Container name (default: cb2000-arch)
  CONTAINER_IMAGE  Override container image (default: docker.io/library/archlinux:latest)
```

### `build_libfprint_rpm_opensuse.sh` — Build RPM for openSUSE / Aeon / MicroOS

Generates a minimal `.spec` and builds an RPM using `rpmbuild` inside an
openSUSE Tumbleweed distrobox container. Provides both the `zypper install`
command for regular openSUSE and the `transactional-update pkg install` tip
for Aeon/MicroOS.

```bash
Usage: ./scripts/build_libfprint_rpm_opensuse.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]

Environment variables:
  LIBFPRINT_DIR        Source tree to package (default: $HOME/libfprint)
  BUILD_DIR            Working build directory (default: $HOME/libfprint-suse-build)
  DBX_NAME             Container name (default: cb2000-suse)
  HOST_ISOLATED_HOME   Override host path in the transactional-update tip
  CONTAINER_IMAGE      Override container image (default: registry.opensuse.org/opensuse/tumbleweed:latest)
```

### `cb2000_collect_runtime.sh` — Collect Runtime Artifacts

Collects the latest CB2000 runtime artifacts into a canonical run folder for
post-session analysis.

```bash
Usage: ./scripts/cb2000_collect_runtime.sh [RUN_ID]
```

### `cb2000_session_log.sh` — Session Log Helper

Small helper called from session scripts to append timestamped events to the
session log.

### `cb2000_backup_iteration.sh` — Backup Driver Iteration

Creates a versioned snapshot of the current driver source files.

## Analysis and Reporting

All analysis launchers load `.cb2000_env` automatically to pick up the correct
`CB2000_LOG_ROOT` when present. If the local Python venv exists, they use it.

### `battery_report.sh` → `cb2000_battery_report.py`

Computes GAR/FAR metrics for a verify session.

```bash
Usage: ./scripts/battery_report.sh [--session SESSION_ID] [--cut HH:MM:SS] [--sigfm]

Examples:
  ./scripts/battery_report.sh
  ./scripts/battery_report.sh --session 20260301_170555
  ./scripts/battery_report.sh --session 20260301_170555 --cut 17:10:00 --sigfm
```

### `gate_summary.sh` → `cb2000_verify_gate_summary.py`

Summarizes verify-side gate decisions and retry causes for a runtime run.

```bash
Usage: ./scripts/gate_summary.sh --run RUN_DIR [options]
```

### `cb2000_enroll_gate_report.py`

Summarizes **enrollment-side** gate behavior from `enroll.log`: stage
progression, retry causes, quality rejects, diversity rejects, and threshold
simulations.

This is useful when tuning the 15-stage feature-mosaic enrollment path and when
explaining why a session stalled before reaching full accepted coverage.

```bash
Usage: ./scripts/cb2000_enroll_gate_report.py [RUN_DIR|enroll.log] [--out FILE]
```

### `verify_metrics.sh` → `cb2000_verify_metrics.py`

Shows detailed SIGFM metrics per verify attempt: probe keypoints, mosaic
keypoints, match count, consensus, and score.

```bash
Usage: ./scripts/verify_metrics.sh --run RUN_DIR [options]
```

### `batch_summary.sh` → `cb2000_verify_batch_summary.py`

Summarizes results across multiple verify attempts in a batch run.

```bash
Usage: ./scripts/batch_summary.sh --run RUN_DIR [options]
```

### Other Python analysis helpers

- `cb2000_run_report.py`
- `cb2000_verify_fn_rootcause.py`

These remain useful for local investigation, but the main release-facing tools
are:

- `battery_report.sh`
- `cb2000_enroll_gate_report.py`
- `gate_summary.sh`
- `verify_metrics.sh`
- `batch_summary.sh`

## Environment Variables Reference

| Variable | Default | Description |
|----------|---------|-------------|
| `CB2000_LOG_ROOT` | `../dev_logs/sessions` | Root directory for session logs |
| `CB2000_CONTAINER_CONFIG_ROOT` | `~/.ContainerConfigs/<DBX_NAME>` | Dedicated per-container lab root |
| `CB2000_LIBFPRINT_ROOT` | `~/.ContainerConfigs/<DBX_NAME>/libfprint` | libfprint source + build tree |
| `CB2000_RUNTIME_ROOT` | `~/.ContainerConfigs/<DBX_NAME>/cb2000_runtime` | Runtime artifacts (images, variants) |
| `DBX_NAME` | `canvasbio` | distrobox container name |
| `CB2000_USE_SUDO` | `0` | Set to 1 to run test binary with sudo |
| `CB2000_AUTO_FIX_USB` | `0` | When `1`, runners call `cb2000_usb_host_access.sh --ensure` before aborting on USB permission failure |
| `CB2000_SESSION_ID` | timestamp | Override session identifier |
| `CB2000_OPENCV_PKG` | auto-detect (`opencv4` preferred) | pkg-config package name for OpenCV |

The orchestrator saves `CB2000_LOG_ROOT`, `CB2000_LIBFPRINT_ROOT`, `DBX_NAME`,
and `BUILD_TARGET` to `.cb2000_env` in the repo root.

## Python Venv

Analysis scripts use a local venv at `scripts/venv/`. Create it with:

```bash
./scripts/cb2000_orchestrator.sh
# then choose option [2]
```

Or manually:

```bash
python3 -m venv scripts/venv
scripts/venv/bin/pip install -r requirements.txt
```

All `*.sh` launchers activate the venv automatically if it exists.
