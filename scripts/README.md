# Scripts Documentation

This directory contains the development-lab helpers for the
`canvasbio-cb2000` driver package.

The active lab assumes a dedicated per-container root under
`~/.ContainerConfigs/<DBX_NAME>/`. In the default container flow:

- `setup/setup-libfprint.sh` uses `~/.ContainerConfigs/<DBX_NAME>/libfprint`
- runtime artifacts go under `~/.ContainerConfigs/<DBX_NAME>/cb2000_runtime`
- distro package builders emit host-facing install tips against that isolated root

These scripts are public on purpose, but they should be read as developer and
tinkerer tooling, not as a polished end-user installer.

Current public snapshot assumptions:

- `R2.5` is a functional technical snapshot, not the final public testing phase
- OpenCV is still required in the active helper path
- enrollment is intentionally `15` stages
- the driver disables libfprint's generic thermal hot-shutdown for this device
- no special workflow tooling is required to use these scripts

The scripts fall into four groups:

- **Orchestration**: the interactive entry point for day-to-day development
- **Integration/build**: prepare libfprint, compile the sidecar helper, rebuild
  the driver
- **Session/runtime**: run enroll/verify sessions and collect artifacts
- **Analysis/reporting**: inspect session logs and summarize gate behavior

## Recommended Workflows

### Canonical session flow

For day-to-day runtime validation, the canonical session entry point is:

```text
run_cb2000_interactive.sh
```

That script is the real executor for the `canvasbio` lab. It owns:

- the default container name (`canvasbio`)
- the isolated lab root under `~/.ContainerConfigs/<DBX_NAME>/`
- the active `libfprint` tree under
  `~/.ContainerConfigs/<DBX_NAME>/libfprint`
- the runtime artifact root under
  `~/.ContainerConfigs/<DBX_NAME>/cb2000_runtime`
- the interactive `capture -> enroll -> verify` loop

The surrounding scripts have narrower roles:

- `setup/setup-libfprint.sh`
  prepares and rebuilds the isolated `libfprint` tree
- `cb2000_orchestrator.sh`
  is a convenience menu that eventually calls
  `run_cb2000_interactive.sh`
- `cb2000_ubuntu_tester.sh`
  is a secondary helper path and should not be treated as the primary runtime
  source of truth for the lab

Operational rule:

- when validating the real lab flow, rebuild with `setup/setup-libfprint.sh`
  and then run `run_cb2000_interactive.sh`
- do not mix host package state, ad-hoc `~/libfprint`, and the isolated
  `~/.ContainerConfigs/<DBX_NAME>/libfprint` tree in the same validation pass

### Known regression traps

These points were observed in real recovery work and should be treated as part
of the current lab contract:

- a freshly recreated Ubuntu container may expose only the `ubuntu` user, while
  the lab expects the host user identity inside the container
- if the user identity is misaligned, `distrobox enter canvasbio` can fail or
  land in a broken runtime layout
- the canonical lab state lives under `~/.ContainerConfigs/canvasbio/`; host
  package installs and ad-hoc trees are not equivalent substitutes
- early bootstrap inside the Ubuntu container may have a broken `sudo`
  environment, so dependency repair can require root execution from the host
- USB permission failures and driver regressions are different failure classes;
  confirm device access before drawing conclusions from enroll or verify output
- if you need to revalidate from zero, prefer:

```text
1. recreate the `canvasbio` container
2. rebuild the isolated libfprint tree with setup/setup-libfprint.sh
3. build the OpenCV helper with packaging/build_sigfm_opencv_helper.sh
4. run run_cb2000_interactive.sh
```

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
1. setup/setup-libfprint.sh --check
2. setup/setup-libfprint.sh --install-deps --clone --integrate --build
3. packaging/build_sigfm_opencv_helper.sh
4. run_cb2000_interactive.sh
```

`setup/setup-libfprint.sh` is for preparing a local libfprint tree. The OpenCV
helper remains a sidecar built from this repo, and the default target tree is
the isolated lab path under `~/.ContainerConfigs/<DBX_NAME>/libfprint`.

### Secondary Ubuntu tester flow

```text
1. cb2000_ubuntu_tester.sh prepare-host
2. cb2000_ubuntu_tester.sh prepare-runtime
3. cb2000_ubuntu_tester.sh interactive
4. cb2000_ubuntu_tester.sh build-deb
```

This path is a secondary helper for Ubuntu-oriented testing, but it is not the
canonical runtime validation path for the lab. It installs the host USB rule,
prepares the dedicated Ubuntu container, validates the driver build, and can
emit a tester-focused `.deb`. The default container name here is `canvasbio`,
and the runtime/build roots stay under `~/.ContainerConfigs/canvasbio/`.

### Packaging flow (distro packages)

Each distro has a dedicated hardened build script. All scripts share the same
container management pattern: they auto-create a named distrobox container on
first run, or accept `--ephemeral` for a throwaway container.

At the end of a successful package build, the scripts also stage a normalized
public-facing release alias under `test/release_assets/<snapshot>/public/` so
the GitHub Release upload step can use stable names independent of distro
version strings. The current stable stem is `libfprint-canvasbio-cb2000`, so
the aliases look like `<distro>_libfprint-canvasbio-cb2000.<extension>`.

The package builders also install the project and third-party notice files into
the package doc path so the runtime artifacts carry the same licensing context
as the source snapshot.

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
./scripts/packaging/build_libfprint_rpm_fedora.sh

# Ubuntu — throwaway container, custom source path
LIBFPRINT_DIR="$HOME/libfprint" ./scripts/packaging/build_libfprint_deb_ubuntu.sh --ephemeral

# Arch — just create the container, build later
./scripts/packaging/build_libfprint_pkg_arch.sh --create-container

# openSUSE — custom container name
DBX_NAME=my-suse ./scripts/packaging/build_libfprint_rpm_opensuse.sh
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

### `setup/setup-libfprint.sh` — Integrate Into a Local libfprint Tree

Copies the CB2000 driver sources into a local libfprint checkout, adds the
driver directory to the libfprint tree when needed, and can trigger a rebuild.

This is the quickest way to prepare a clean local integration for release
verification. Works on the host directly — no container required.

```bash
Usage: ./scripts/setup/setup-libfprint.sh [options]

Options:
  --install-deps    Install build dependencies (Ubuntu/Debian)
  --clone           Clone or update libfprint
  --integrate       Copy driver files into libfprint/libfprint/drivers/canvasbio_cb2000
  --build           Build libfprint with canvasbio_cb2000 enabled
  --check           Syntax-check local driver source files
  --udev            Install/reload udev rule for CB2000 USB access (requires sudo)
  --all             Run install-deps + clone + integrate + build + udev

Environment variables:
  LIBFPRINT_DIR     Path to libfprint source tree
                    (default: ~/.ContainerConfigs/$DBX_NAME/libfprint)
```

Notes:

- It integrates the main C driver sources only.
- The feature-mosaic OpenCV helper is built separately by
  `packaging/build_sigfm_opencv_helper.sh`.
- The optional udev rule lives in `tools/99-canvasbio.rules`.

### `build_sigfm_opencv_helper.sh` — Build OpenCV Helper

Compiles `cb2000_sigfm_opencv_helper.cpp` into
`build/libcb2000_sigfm_opencv.so`.

```bash
Usage: bash scripts/packaging/build_sigfm_opencv_helper.sh

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

### `cb2000_usb_host_access.sh` — Host USB Rule Helper

Checks whether the CB2000 device node is writable and, when needed, installs
`tools/99-canvasbio.rules` on the host before reloading `udev`.

```bash
Usage: ./scripts/setup/cb2000_usb_host_access.sh [--check | --install-host-rule | --ensure]
```

### `cb2000_ubuntu_tester.sh` — Ubuntu Tester Entry Point

Single entry point for low-friction tester workflows on Ubuntu-based setups.
Runs on the host; delegates into the `canvasbio` distrobox container as needed.

```bash
Usage: ./scripts/cb2000_ubuntu_tester.sh [prepare-host | prepare-runtime | interactive | build-deb | all]

Actions:
  prepare-host     Install/update the CB2000 udev rule on the host (needs sudo)
  prepare-runtime  Create the Ubuntu container, install deps, and build the environment
  interactive      Run the interactive development session
  build-deb        Build the Ubuntu .deb package
  all              prepare-host + prepare-runtime + build-deb
```

### `build_libfprint_rpm_fedora.sh` — Build RPM for Fedora / Kinoite / Silverblue

Builds a custom libfprint RPM inside a Fedora 43 distrobox container.
Uses `fedpkg` to pull the official `.spec`, injects the local source tree as a
custom tarball, and bumps the release tag to `99.canvasbio.<timestamp>` so it
supersedes the distro package.

On atomic desktops (Kinoite/Silverblue) the output tip provides the full
`rpm-ostree override replace` command to run on the host.

```bash
Usage: ./scripts/packaging/build_libfprint_rpm_fedora.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]

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
Usage: ./scripts/packaging/build_libfprint_deb_ubuntu.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]

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
Usage: ./scripts/packaging/build_libfprint_pkg_arch.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]

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
Usage: ./scripts/packaging/build_libfprint_rpm_opensuse.sh [--in-container | --no-container | --ephemeral | --persistent | --create-container]

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

### `cb2000_prepare_release_asset.sh` — Normalize Release Attachment Names

Creates a public-facing copy of a built package using the stable attachment
shape `<distro>_libfprint-canvasbio-cb2000.<extension>`.

Examples:

```bash
./scripts/cb2000_prepare_release_asset.sh \
  --distro ubuntu-debian \
  --package-name libfprint-2-2-canvasbio \
  --public-name libfprint-canvasbio-cb2000 \
  --artifact "$HOME/libfprint-deb-build/libfprint-2-2-canvasbio_1.94.10+canvasbio.202604121230_amd64.deb"
```

The packaging scripts call this automatically after a successful build. The
default target is:

```text
test/release_assets/R2.5/public/
```

### `cb2000_session_log.sh` — Session Log Helper

Small helper called from session scripts to append timestamped events to the
session log.

### `metrics/analyze.sh` — All Analysis Reports

Single entry point for all CB2000 metrics and analysis. Handles venv activation
and `CB2000_LOG_ROOT` automatically.

```bash
Usage: ./scripts/metrics/analyze.sh <command> [options]

Commands:
  run-report    --run <run_dir>   Full per-run telemetry and verify report
  gate-summary  --run <run_dir>   Finalize-ACK and ready-matrix routing summary
  metrics       --run <run_dir>   Matcher decision counts and rates
  enroll-gate   <run_dir>         Enrollment gate diagnostics and diversity analysis
  fn-rootcause  --run <run_dir>   Root-cause classifier for false negatives
  batch-summary                   Cross-run batch verify summary table
  battery       [options]         GAR/FAR session battery report
  all           --run <run_dir>   Run all per-run reports in sequence, then cross-run
```

Python scripts called by each subcommand:

| Command | Python script | Purpose |
|---|---|---|
| `run-report` | `cb2000_run_report.py` | Full per-run telemetry report |
| `gate-summary` | `cb2000_verify_gate_summary.py` | Finalize-ACK routing summary |
| `metrics` | `cb2000_verify_metrics.py` | SIGFM decision counts and rates |
| `enroll-gate` | `cb2000_enroll_gate_report.py` | Enrollment gate diagnostics |
| `fn-rootcause` | `cb2000_verify_fn_rootcause.py` | False-negative root cause |
| `batch-summary` | `cb2000_verify_batch_summary.py` | Cross-run batch table |
| `battery` | `cb2000_battery_report.py` | GAR/FAR session battery |

Examples:

```bash
# run all reports for a specific session
./scripts/metrics/analyze.sh all --run ~/.ContainerConfigs/canvasbio/cb2000_runtime/runs/20260412_153000

# single report
./scripts/metrics/analyze.sh battery
./scripts/metrics/analyze.sh gate-summary --run <run_dir>

# enrollment gate diagnostics (positional run_dir, no --run flag)
./scripts/metrics/analyze.sh enroll-gate <run_dir>
```

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
scripts/venv/bin/pip install -r scripts/requirements.txt
```

All `*.sh` launchers activate the venv automatically if it exists.
