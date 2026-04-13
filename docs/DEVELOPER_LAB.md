# Developer Lab

This repository keeps a development lab on purpose.

It is meant for:

- developers who want to improve the driver
- tinkerers who want to rebuild, test and inspect behavior
- packagers who want distro-native artifacts
- contributors who need real runtime and packaging helpers

It is not framed as a polished consumer installer.

## Scope

The lab covers:

- integration into a local `libfprint` tree
- sidecar helper builds
- runtime session helpers
- USB permission helpers
- report generation
- distro package generation

The scripts are there to reduce friction for technical contributors, not to
promise a universal one-command install experience for all users.

## Where To Start

- overall script catalog:
  `scripts/README.md`
- driver rationale:
  `docs/DRIVER_NOTES.md`
- package download and install guidance:
  `releases/README.md`

## Canonical Session Flow

The canonical runtime entry point for the lab is:

- `scripts/run_cb2000_interactive.sh`

That script is the real session executor. It is the source of truth for:

- the default container name (`canvasbio`)
- the isolated lab root under `~/.ContainerConfigs/<DBX_NAME>/`
- the active `libfprint` tree under
  `~/.ContainerConfigs/<DBX_NAME>/libfprint`
- the runtime artifact root under
  `~/.ContainerConfigs/<DBX_NAME>/cb2000_runtime`
- the interactive `capture -> enroll -> verify` session flow

Supporting roles:

- `scripts/setup/setup-libfprint.sh`
  prepares and rebuilds the isolated `libfprint` tree used by the session
- `scripts/cb2000_orchestrator.sh`
  is only a convenience menu that eventually delegates to
  `run_cb2000_interactive.sh`
- `scripts/cb2000_ubuntu_tester.sh`
  is not the canonical validation path for the lab and should not be treated as
  the primary source of truth for session behavior

Operational rule:

- when validating the live development flow, rebuild the isolated
  `libfprint` tree with `scripts/setup/setup-libfprint.sh` and then run
  `run_cb2000_interactive.sh`
- do not mix host package state, ad-hoc `~/libfprint` trees, and the
  `~/.ContainerConfigs/<DBX_NAME>/libfprint` lab tree in the same validation
  pass

## Recent Findings

The current lab has a few sharp edges that must be treated as known operational
constraints:

- a fresh Ubuntu distrobox can come up with only an `ubuntu` user even when the
  lab expects the host user name inside the container
- if that happens, `distrobox enter <name>` may fail until the container user
  is aligned with the host account and home path
- the canonical lab container is `canvasbio`, and the canonical isolated roots
  remain:
  - `~/.ContainerConfigs/canvasbio/libfprint`
  - `~/.ContainerConfigs/canvasbio/cb2000_runtime`
- during recovery or rebuild work, do not treat a random host-installed
  `libfprint` package as equivalent to the isolated lab tree
- on the current Ubuntu container bootstrap path, `sudo` inside the container
  is not a safe assumption during early setup; root-side package repair may
  need to happen from the host with `distrobox-host-exec` and `podman exec`
- the canonical validation script is still
  `scripts/run_cb2000_interactive.sh`; menu wrappers and secondary tester
  helpers should not be used to redefine what "the real lab flow" means
- USB access debugging must be interpreted separately from driver logic; a bad
  device permission state on the host can invalidate a session before matcher
  behavior is even relevant

Practical consequence:

- when a session behaves strangely, first confirm the active container, active
  isolated `libfprint` tree, host USB access state, and whether the flow came
  from `run_cb2000_interactive.sh`

## Status Label For Scripts

The correct framing for the `scripts/` tree is:

- developer lab
- operational helpers
- packaging helpers

Not:

- official end-user installer
- guaranteed zero-friction production deployment tool
