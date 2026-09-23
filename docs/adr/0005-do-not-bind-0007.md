# 0005. Do not bind 2df0:0007

Status: accepted

## Context

`2df0:0007` is the fingerprint sensor of the Galaxy Book5 360, and users
reasonably assume it is a variant of `2df0:0003`. It is not. It is a Realtek
match-on-chip sensor: its Windows package is named `RtsMocWbdi` and contains
SDCP client functions, the matching happens inside the chip, and the host
never sees an image. Its USB descriptors differ too (a different firmware
generation, four endpoints instead of three, Microsoft OS 2.0 descriptors),
and when this driver was pointed at one it stalled on the first GPIO write of
the wake sequence and then looped on USB resets.

An early version of this driver claimed both product identifiers.

## Decision

The driver binds `2df0:0003` only. `2df0:0007` is not claimed, and every
document that mentions it sends the reader to the catalogue that tracks that
device rather than only declaring it out of scope.

## Consequences

- A Galaxy Book5 360 gets a clean "no driver found" instead of a device that
  looks broken.
- Users of that machine get a place to go: the
  [linux-fingerprint-drivers](https://github.com/jedbillyb/linux-fingerprint-drivers)
  catalogue, where the device is tracked, and libfprint's own `realtek`
  driver, where match-on-chip support belongs.
- Re-adding it would need a per identifier command path validated on real
  `2df0:0007` hardware, which nobody here has.
- A match-on-chip device decides internally whether a finger matches, so
  nothing in this driver, from the command tables to the engine, would apply
  to it.

## Evidence

`src/cb2000_protocol.h`, the comment on the product identifiers.
`docs/PROTOCOL.md`, "Descriptors: 2df0:0003 versus 2df0:0007".
Reporter evidence in
[issue #3](https://github.com/kpagnussat/canvasbio-cb2000/issues/3).
