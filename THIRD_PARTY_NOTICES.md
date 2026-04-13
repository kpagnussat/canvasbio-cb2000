# Third-Party Notices

This repository contains project-authored code plus specific third-party
derived/adapted portions. Project-authored source files are distributed under
`LGPL-2.1-or-later` as indicated in the relevant file headers. Third-party
notices that must remain preserved are listed below.

## libfprint (host framework and packaging base)

- Upstream project: `https://gitlab.freedesktop.org/libfprint/libfprint`
- Role: target framework, ABI host and upstream packaging base used by the current distro builders
- Distribution model in this repo: the libfprint source itself is not vendored here as part of the standalone driver snapshot, but the package builders clone/use the upstream libfprint tree and integrate the CB2000 driver into that build

## SIGFM (algorithmic reference used for matcher parity work)

- Upstream repository: `https://github.com/goodix-fp-linux-dev/sigfm`
- Upstream commit: `dc591ca03276348ad731ab78c8d722fa40b3158f`
- Copyright: Goodix Fingerprint Linux Development
- Upstream license: MIT

Portions of the matcher behavior and helper validation logic in:

- `src/cb2000_sigfm_matcher.c`
- `src/cb2000_sigfm_opencv_helper.cpp`

are derived/adapted from the SIGFM upstream reference.

The required upstream MIT notice is preserved in:

- `third_party_licenses/SIGFM-MIT.txt`

## OpenCV (current external dependency)

- Component: OpenCV, used by the active helper `libcb2000_sigfm_opencv.so`
- Scope: current `R2.5` feature-mosaic enrollment/verify path
- Distribution model in this repo: external dependency via distro packages, not vendored OpenCV source

Upstream OpenCV licensing depends on the exact OpenCV version shipped by the
target distro:

- OpenCV `4.5.0` and higher: Apache 2.0
- OpenCV `4.4.0` and lower: BSD 3-Clause

For this project that means:

- OpenCV is still required at build/runtime in `R2.5`
- linking against the distro OpenCV package does not by itself relicense the
  project-authored source files in this repository
- downstream users should also comply with the OpenCV package license shipped by
  their target distro
