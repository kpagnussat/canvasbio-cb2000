# 0003. Ship as a libfprint TOD module, not as a libfprint build

Status: accepted

## Context

The first public snapshot (R2.5) shipped a package named `libfprint-2-2`
that replaced the distribution's libfprint with a build that had this driver
compiled inside it. That works, and it has three costs: the system library
stops receiving its distribution's updates, including security ones; any
other fingerprint reader on the machine depends on our build; and the user
has no way to tell from the package name what happened to their system.

libfprint supports Touch OEM Drivers (TOD), a mechanism where the library
loads driver modules out of a directory at runtime. It is a light fork
maintained outside upstream libfprint, and it is what every vendor supplied
fingerprint driver on Linux uses.

## Decision

The supported artifact is a TOD module: one shared object exporting one
symbol, installed into the TOD drivers directory, depending on the
distribution's TOD enabled libfprint. The distribution's libfprint stays in
place.

## Consequences

- Other readers keep working, and the system library keeps its updates.
- The driver depends on a libfprint with TOD support being available, which
  is true on Ubuntu and openSUSE out of the box, and needs a third party
  repository on Fedora and Arch. The README says so per distribution.
- `TOD_DRIVERS_DIR` is fixed at libfprint's compile time and differs per
  distribution, so a tarball with a loose module cannot work anywhere. Each
  distribution gets its own package, built against its own
  `libfprint-2-tod-1.pc`.
- Only `fpi_tod_shared_driver_get_type` is exported, enforced by a linker
  version script, so nothing in the module can collide with another module
  loaded by the same process.
- Anyone coming from R2.5 must restore their distribution's libfprint first,
  because that old build does not load TOD modules at all and will silently
  ignore this one.

## Evidence

`src/cb2000_tod_entry.c`, `src/cb2000_tod.map`, `meson.build`,
`packaging/debian/`. The R2.5 package metadata is quoted in the README and
in `CHANGELOG.md`.
