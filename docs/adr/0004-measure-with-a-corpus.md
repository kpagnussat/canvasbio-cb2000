# 0004. Measure with a private corpus and an offline harness

Status: accepted

## Context

For about a year the matcher was tuned and evaluated by live testing on the
sensor: change a gate, touch, record the result. Under that method two
versions of the same matcher reported 25% genuine accepts with 14% false
accepts and then 81% with none, with no way to tell how much of the
difference came from the code and how much from the person placing the
finger. Then a hardware test accepted a finger from the other hand twice
out of five attempts, on a build that was nearly packaged for release.

That was the point where live testing alone stopped being merely imprecise and
became a release risk.

## Decision

Nothing about matching is decided by touching the sensor. A private corpus of
raw frames is recorded once, and every change is measured by replaying that
corpus through an offline harness that links the driver's own decision code
instead of a copy of it. Every genuine accept rate is reported next to the
impostor rate from the same run.

## Consequences

- A change is measured in minutes over thousands of comparisons, and a
  regression cannot hide behind a lucky placement.
- The decision code must stay independent of USB and of libfprint, so the
  harness can link it. That constraint shapes the source layout.
- The corpus is one person's biometric data. It lives in an encrypted volume
  outside this repository and is never published, which means the numbers
  here can be reproduced in method but not in data. See
  [0011](0011-measure-behavioral-agreement.md) for the same trade on the
  oracle side.
- The release that was nearly packaged did not ship.

## Evidence

`tests/offline/cb2000-harness.c` and `tests/offline/cb2000-collect.c`, the
corpus tables in `docs/RESEARCH.md`, and `src/cb2000_core.{c,h}` plus
`src/cb2000_engine*` which contain no libfprint and no USB.
