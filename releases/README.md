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

| Distro family | Internal package name | Current release asset |
|---------------|-----------------------|------------------------|
| Ubuntu / Debian | `libfprint-2-2-canvasbio` | `libfprint-2-2-canvasbio_1.94.10+canvasbio.202604121230_amd64.deb` |
| Fedora / Kinoite / Silverblue | `libfprint` | `libfprint-1.94.10-99.canvasbio.202604121232.fc43.x86_64.rpm` |
| Arch Linux | `libfprint-canvasbio` | `libfprint-canvasbio-1.94.10-202604121230-x86_64.pkg.tar.zst` |
| openSUSE Tumbleweed / Aeon / MicroOS | `libfprint2-canvasbio` | `libfprint2-canvasbio-1.94.10-99.canvasbio.202604121233.x86_64.rpm` |

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
package per distro family, clearly documented, with exact filenames listed
above.

## Public Docs Linkage

The public documentation should point here when users ask:

- where do I download packages?
- which package format should I install?
- are prebuilt packages available for my distro?
