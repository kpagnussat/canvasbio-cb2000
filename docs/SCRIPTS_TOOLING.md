# Scripts Tooling

This document covers the public operational scripts that ship with the current
snapshot.

## Execution Flow

Use this table to find the right script sequence for your goal.

| Goal | Script / Command | Notes |
|---|---|---|
| First-time host setup | `setup-libfprint.sh --all` | Installs deps, clones libfprint, integrates driver, builds, sets up udev. Requires sudo for udev step. Run in an interactive terminal. |
| USB device not accessible | `setup-libfprint.sh --udev` | Installs or reloads `tools/99-canvasbio.rules` on the host. Requires sudo. |
| Rebuild driver only | `setup-libfprint.sh --integrate --build` | Re-copies driver files and rebuilds libfprint. No sudo needed. |
| Interactive dev session | `run_cb2000_interactive.sh` | Builds helper + driver, runs capture → enroll → verify with USB preflight. |
| Build OpenCV helper only | `build_sigfm_opencv_helper.sh` | Builds `libcb2000_sigfm_opencv.so`. Required before running sessions. |
| Check USB access | `cb2000_usb_host_access.sh --check` | Returns 0 (ok), 10 (not found), or 11 (found, not writable). |
| Fix USB access only | `cb2000_usb_host_access.sh --ensure` | Installs rule if missing, reloads rules, triggers device. Requires sudo. |
| All per-run analysis | `metrics/analyze.sh all --run <run_dir>` | Runs all reports in sequence: run-report → gate-summary → metrics → enroll-gate → fn-rootcause → batch-summary → battery. |
| Per-run verify report | `metrics/analyze.sh run-report --run <run_dir>` | Full per-run telemetry and verify report. |
| Gate summary | `metrics/analyze.sh gate-summary --run <run_dir>` | Finalize-ACK and ready-matrix routing summary. |
| Verify metrics | `metrics/analyze.sh metrics --run <run_dir>` | Matcher decision counts and rates. |
| Enrollment gate | `metrics/analyze.sh enroll-gate <run_dir>` | Enrollment gate diagnostics and diversity analysis. |
| False-negative root cause | `metrics/analyze.sh fn-rootcause --run <run_dir>` | Root-cause classifier for false negatives. |
| Cross-run batch summary | `metrics/analyze.sh batch-summary` | Aggregated verify table across multiple runs. |
| GAR/FAR battery report | `metrics/analyze.sh battery` | Session battery aggregation across collected logs. |
| Build Ubuntu/Debian package | `build_libfprint_deb_ubuntu.sh` | Produces `.deb` from the local source tree. |
| Build Fedora/Kinoite package | `build_libfprint_rpm_fedora.sh` | Produces `.rpm` from the local source tree. |
| Build Arch package | `build_libfprint_pkg_arch.sh` | Produces `.pkg.tar.zst` from the local source tree. |
| Build openSUSE package | `build_libfprint_rpm_opensuse.sh` | Produces `.rpm` from the local source tree. |

### Typical First-Time Flow

```
./scripts/setup/setup-libfprint.sh --all                    # 1. environment + udev setup (needs sudo)
./scripts/run_cb2000_interactive.sh                   # 2. capture → enroll → verify
./scripts/metrics/analyze.sh all --run <run_dir>             # 3. all per-run reports in sequence
./scripts/metrics/analyze.sh batch-summary                   # 4. cross-run batch table
./scripts/metrics/analyze.sh battery                         # 5. GAR/FAR battery report
```

### Typical Dev Iteration Flow

```
./scripts/run_cb2000_interactive.sh                   # 1. rebuild driver + run session
./scripts/metrics/analyze.sh all --run <run_dir>             # 2. all per-run reports
./scripts/metrics/analyze.sh battery                         # 3. GAR/FAR cross-run report
```

### Session Annotation

```
cb2000_session_log.sh "<cmd>" "<result>" "<error_sig>" "<decision>"
```

Use to record commands, results, and decisions during a session.
State is appended to `docs/cb2000_session_state.md`.

## Main Entry Points

- `scripts/setup/setup-libfprint.sh`
  - integrates the driver into a local libfprint tree and patches Meson
- `scripts/cb2000_orchestrator.sh`
  - interactive development entry point for helper build, driver rebuild, and
    enroll/verify sessions
- `scripts/cb2000_ubuntu_tester.sh`
  - tester-facing path for host USB preparation, runtime preparation, real test
    runs, and `.deb` generation
- `scripts/setup/cb2000_usb_host_access.sh`
  - checks USB access and installs `tools/99-canvasbio.rules` on the host when
    needed

## Runtime and Build Helpers

- `scripts/packaging/build_sigfm_opencv_helper.sh`
  - builds the required helper `libcb2000_sigfm_opencv.so`
- `scripts/run_cb2000_interactive.sh`
  - interactive runtime flow with helper build, libfprint rebuild, and USB
    access preflight

## Packaging Scripts

- `scripts/packaging/build_libfprint_deb_ubuntu.sh`
  - builds the Ubuntu/Debian `.deb`
- `scripts/packaging/build_libfprint_rpm_fedora.sh`
  - builds the Fedora/Kinoite/Silverblue `.rpm`
- `scripts/packaging/build_libfprint_pkg_arch.sh`
  - builds the Arch `.pkg.tar.zst`
- `scripts/packaging/build_libfprint_rpm_opensuse.sh`
  - builds the openSUSE/Aeon/MicroOS `.rpm`
- `scripts/cb2000_prepare_release_asset.sh`
  - stages normalized public release aliases under
    `test/release_assets/<snapshot>/public/`

## Reporting and Analysis

`scripts/metrics/analyze.sh <command> [options]` — single entry point for all analysis.
Handles venv activation and `CB2000_LOG_ROOT` automatically.

| Command | Python script | Purpose |
|---|---|---|
| `all --run <dir>` | (all below) | Run every report in sequence |
| `run-report --run <dir>` | `cb2000_run_report.py` | Full per-run telemetry report |
| `gate-summary --run <dir>` | `cb2000_verify_gate_summary.py` | Finalize-ACK routing summary |
| `metrics --run <dir>` | `cb2000_verify_metrics.py` | Decision counts and rates |
| `enroll-gate <dir>` | `cb2000_enroll_gate_report.py` | Enrollment gate diagnostics |
| `fn-rootcause --run <dir>` | `cb2000_verify_fn_rootcause.py` | False-negative root cause |
| `batch-summary` | `cb2000_verify_batch_summary.py` | Cross-run batch table |
| `battery` | `cb2000_battery_report.py` | GAR/FAR session battery |

Other utilities:

- `scripts/cb2000_collect_runtime.sh` — runtime artifact collection
- `scripts/cb2000_session_log.sh` — session annotation helper

## Runtime Path Conventions

The public lab expects a dedicated container root under:

- `~/.ContainerConfigs/<DBX_NAME>/`

Important derived paths:

- libfprint root:
  `~/.ContainerConfigs/<DBX_NAME>/libfprint`
- runtime root:
  `~/.ContainerConfigs/<DBX_NAME>/cb2000_runtime`
- default session logs:
  `../dev_logs/sessions/` relative to the checkout, unless `CB2000_LOG_ROOT`
  overrides it

Packaging helpers for Fedora and openSUSE also resolve host install tips from
`HOST_ISOLATED_HOME`, which defaults to the same isolated container root.

## Dependency Model

The current public scripts assume the OpenCV helper is part of the active
runtime path:

- `src/cb2000_sigfm_matcher.c` remains the in-driver matcher core
- `src/cb2000_sigfm_opencv_helper.cpp` builds the helper used by the mosaic path
- runtime helpers build and export `libcb2000_sigfm_opencv.so` before testing

## Public Scope
