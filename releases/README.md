# Releases

This directory documents the installable package outputs that should be attached
to project releases.

The repository does not need to store every built package directly in git. The
recommended public distribution model is:

- commit source code and documentation to the repository
- publish built artifacts in GitHub Releases
- reference those artifacts from here and from the main `README.md`

## Snapshot Positioning

The public repository is currently intended to publish a functional `R2.5`
snapshot. That means:

- the packages are useful for reproduction and technical evaluation
- the current dependency set is still accepted as-is
- OpenCV is still required in the active runtime path
- enrollment behavior is the same `15`-stage flow described in the source/docs
- the driver ships with the CB2000-specific thermal override already enabled
- this is not yet the final dependency-reduced shape intended for wider public
  testing and upstream-facing work

## Package Families

- Ubuntu/Debian:
  `.deb`
- Fedora/Kinoite/Silverblue:
  `.rpm`
- Arch Linux:
  `.pkg.tar.zst`
- openSUSE Tumbleweed/Aeon/MicroOS:
  `.rpm`

## R2.5 Package Set

These are the runtime packages currently produced and suitable for GitHub
release attachments for the `R2.5` snapshot:

| Distro family | Internal package name | Public release attachment |
|---------------|-----------------------|---------------------------|
| Ubuntu / Debian | `libfprint-2-2-canvasbio` | `ubuntu-debian_libfprint-canvasbio-cb2000.deb` |
| Fedora / Kinoite / Silverblue | `libfprint` | `fedora_libfprint-canvasbio-cb2000.rpm` |
| Arch Linux | `libfprint-canvasbio` | `arch_libfprint-canvasbio-cb2000.pkg.tar.zst` |
| openSUSE Tumbleweed / Aeon / MicroOS | `libfprint2-canvasbio` | `opensuse_libfprint-canvasbio-cb2000.rpm` |

Artifacts that should normally stay out of the public release page:

- `*-debuginfo-*`
- `*-debugsource-*`
- `*-devel-*`
- `*-tests-*`
- Arch `*-debug-*`

## Expected Release Contents

Each release should ideally provide:

- one short summary of the driver state
- notes about important behavior changes
- the available distro packages
- one runtime package per supported distro family, with no debug/devel/test
  artifacts mixed into the release page
- installation notes for atomic desktops when relevant
- an explicit statement that `R2.5` is a functional snapshot, not yet the final
  dependency-reduced public testing phase

## Installation Notes

- Ubuntu/Debian:
  install the published `.deb`
- Fedora atomic desktops:
  use the published `.rpm` with `rpm-ostree override replace` when needed
- Arch Linux:
  install the published `.pkg.tar.zst` with `pacman -U`
- openSUSE regular:
  install the published `.rpm` with `zypper`
- openSUSE Aeon/MicroOS:
  use `transactional-update pkg install`

The published runtime packages declare their runtime dependencies. On supported
systems, the native package manager should therefore pull the required OpenCV
runtime stack automatically. Manual source builds still require the OpenCV
development package.

Each runtime package should also carry the project license and third-party
notice files in its doc path so the package remains self-describing outside the
git repository.

## Approximate Installed Footprint

The release assets themselves are small. The larger footprint comes from the
current runtime dependency stack, especially the OpenCV-based helper path still
used in `R2.5`.

These measurements were taken from clean test containers, so they represent an
approximate fresh-install upper bound. On real systems, the incremental install
may be smaller if part of the dependency stack is already present.

| Distro | Download asset | Custom package installed | Dependencies installed | Approximate total |
|--------|----------------|--------------------------|------------------------|-------------------|
| Ubuntu / Debian | `0.49 MiB` | `1.30 MiB` | `22.16 MiB` | `23.46 MiB` |
| Arch Linux | `0.39 MiB` | `0.85 MiB` | `584.69 MiB` | `585.54 MiB` |
| Fedora / Kinoite / Silverblue | `0.38 MiB` | `0.93 MiB` | `1843.47 MiB` | `1844.40 MiB` |
| openSUSE Tumbleweed / Aeon / MicroOS | `0.34 MiB` | `0.91 MiB` | `27.80 MiB` | `28.71 MiB` |

What the custom package actually contains:

- the custom `libfprint` runtime built with the CanvasBio CB2000 driver integrated
- the current `libcb2000_sigfm_opencv.so` helper
- USB permission rule(s)
- runtime integration files required by the target distro

What it does not contain:

- driver source code
- development headers
- pkg-config files
- test payloads
- debug or devel artifacts

The important practical point is that the driver itself is small. The current
footprint problem is not the CB2000 driver code, but the fact that `R2.5`
still depends on the OpenCV-based helper path. That is one of the main reasons
dependency reduction remains a next-step objective.

## Naming

The current builders generate distro-native names on purpose:

- Ubuntu/Debian:
  `libfprint-2-2-canvasbio_...amd64.deb`
- Fedora:
  `libfprint-...fcXX.x86_64.rpm`
- Arch:
  `libfprint-canvasbio-...x86_64.pkg.tar.zst`
- openSUSE:
  `libfprint2-canvasbio-...x86_64.rpm`

These names are not fully identical across distros because each package needs
to fit the local package manager's replacement and dependency model. What is
normalized for public release purposes is the deliverable set: one runtime
package per distro family, clearly documented, with a single project-stable
public attachment stem listed above.

## Building Packages from Source

Each distro has a dedicated script under `scripts/packaging/`:

| Distro family | Script |
|---|---|
| Ubuntu / Debian | `scripts/packaging/build_libfprint_deb_ubuntu.sh` |
| Fedora / Kinoite / Silverblue | `scripts/packaging/build_libfprint_rpm_fedora.sh` |
| Arch Linux | `scripts/packaging/build_libfprint_pkg_arch.sh` |
| openSUSE Tumbleweed / Aeon / MicroOS | `scripts/packaging/build_libfprint_rpm_opensuse.sh` |

All scripts auto-create a named distrobox container on first run. Alternatively,
pass `--no-container` to build directly on the host when the distro toolchain is
already present.

See `scripts/README.md` for the full packaging flow and supported flags.

## Public Docs Linkage

The public documentation should point here when users ask:

- where do I download packages?
- which package format should I install?
- are prebuilt packages available for my distro?
