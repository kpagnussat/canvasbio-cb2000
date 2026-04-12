# Scripts Tooling

This document covers the active operational scripts only.

## Main scripts

- `scripts/setup-libfprint.sh`
  - Prepares tooling, clones/integrates the driver into the isolated libfprint tree and patches Meson automatically.
- `scripts/cb2000_orchestrator.sh`
  - Interactive development entry point. Persists `.cb2000_env`, resolves container libfprint path automatically and dispatches build/session helpers.
- `scripts/cb2000_ubuntu_tester.sh`
  - Tester-facing entry point for host USB preparation, runtime setup, real test runs and `.deb` generation.
- `scripts/cb2000_usb_host_access.sh`
  - Checks USB writability for the CB2000 and installs `tools/99-canvasbio.rules` on the host when needed.
- `scripts/run_cb2000_interactive.sh`
  - Interactive flow with mandatory OpenCV helper build, libfprint rebuild and USB access preflight.
- `scripts/run_cb2000_test.sh`
  - Non-interactive battery with mandatory OpenCV helper build, libfprint rebuild and USB access preflight.
- `scripts/cb2000_run_report.py`
  - Consolidated verify report from run logs.
- `scripts/cb2000_verify_gate_summary.py`
  - Gate/matrix summary from verify logs.
- `scripts/cb2000_verify_metrics.py`
  - Decision counts/rates from matcher decision lines.
- `scripts/cb2000_enroll_gate_report.py`
  - Enroll quality/diversity gate diagnostics.
- `scripts/cb2000_sigfm_offline_parity.py`
  - Offline parity helper for SIGFM-related comparisons.
- `scripts/cb2000_verify_batch_summary.py`
  - Cross-run batch verify summary table.
- `scripts/cb2000_verify_fn_rootcause.py`
  - False-negative root-cause helper.
- `scripts/cb2000_battery_report.py`
  - Battery/session summary across collected runs.
- `scripts/cb2000_collect_runtime.sh`
  - Collect runtime artifacts from the container workspace.
- `scripts/cb2000_session_log.sh`
  - Session log helper.

## Runtime path conventions

Scripts resolve paths from the current checkout (`PROJECT_ROOT`).

Configurable roots:

- `CB2000_CONTAINER_CONFIG_ROOT` (default `${HOME}/.ContainerConfigs/${DBX_NAME}`)
- `CB2000_LIBFPRINT_ROOT` (default `${CB2000_CONTAINER_CONFIG_ROOT}/libfprint`)
- `CB2000_RUNTIME_ROOT` (default `${CB2000_CONTAINER_CONFIG_ROOT}/cb2000_runtime`)

Packaging helpers for Fedora and openSUSE also resolve host install tips from
that isolated root via `HOST_ISOLATED_HOME`, so the command shown at the end of
the build is directly usable from the host.

Runtime runs may be copied or materialized under `logs/` when scripts are executed, but `logs/` is not part of the active tracked tree.

## Matcher dependency model

Current flow is hybrid:

- `src/cb2000_sigfm_matcher.c` remains the in-driver SIGFM core
- `src/cb2000_sigfm_opencv_helper.cpp` is still required as `libcb2000_sigfm_opencv.so`
- `run_cb2000_interactive.sh` and `run_cb2000_test.sh` build and export the helper before runtime
- Logs and reports still rely on SIGFM decision lines (`[SIGFM_MATCH]`)
