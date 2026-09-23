# 0012. One source tarball, four packages, each built on its own distribution

Status: accepted

## Context

0003 settled that the artifact is a TOD module and that a loose module in a
tarball cannot work, because `tod_driversdir` is fixed when libfprint is
compiled and differs between distributions. What was still open is how the
packages for distributions other than Ubuntu get built, and what they are
allowed to assume about the machine they land on.

The four distributions are not in the same position:

- Ubuntu ships `libfprint-2-tod1`, and the `.deb` is the tested path.
- openSUSE Tumbleweed ships `libfprint-2-2` built with TOD in the OSS
  repository, so a module is all that is needed and the system libfprint is
  untouched.
- Fedora ships no TOD support at all. The library comes from a third party
  COPR and conflicts with the distribution's `libfprint`, and on top of that
  the default SELinux policy stops `fprintd` from loading any TOD module.
- Arch has `libfprint-tod` in the AUR, which also conflicts with
  `libfprint`. There the release is a `PKGBUILD`, not a built package.

## Decision

One reproducible source tarball per release, built from the commit being
released, and every package built from it in a container of its own
distribution, against that distribution's own `libfprint-2-tod-1.pc`. No
`.so` is ever copied from one package to another. One version string, checked
across `meson.build`, the Debian changelog, the spec and the `PKGBUILD`
before any build runs.

On Fedora the SELinux policy is a hard dependency of the package, not a line
in the README.

## Consequences

- A build from a tarball has no checkout to ask for its commit, so
  `make-tarball.sh` writes it into `.commit` and every package of a release
  reports the same version, which is what a bug report needs.
- The library itself is an automatic dependency on `libfprint-2-tod.so.1`,
  which is satisfied by any libfprint built with TOD, whatever the
  distribution calls the package.
- The Fedora package installs only where the policy is also available, which
  in practice means one specific COPR. That is a real restriction, taken on
  purpose: without the policy the module installs, the reader stays dead, and
  nothing tells the user why. A silent failure is worse than an unmet
  dependency.
- Three distributions are packaged without a machine to test them on. They
  are labelled as such in the README, and a report that one works is asked
  for there.
- The Arch checksum cannot live in the tree, because the tarball is made from
  the tree; `build-arch.sh` fills it in on the copy that goes to the release.

## Evidence

`packaging/make-tarball.sh`, `packaging/version.sh`,
`packaging/rpm/libfprint-2-tod1-canvasbio-cb2000.spec`,
`packaging/arch/PKGBUILD`, and the per distribution instructions in
`README.md`.
