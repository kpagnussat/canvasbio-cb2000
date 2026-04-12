# Driver Notes

This document explains the current public-facing behavior of the CanvasBio
CB2000 driver.

## Device Summary

- USB IDs: `0x2DF0:0x0003`, `0x2DF0:0x0007`
- Sensor frame: `80x64`
- Effective resolution: about `340 dpi`
- Active area: `6.0 mm x 4.8 mm`

The sensor is small. That shapes almost every design choice in the enrollment
and matching flow.

## Enrollment Model

The driver currently uses `15` enrollment stages:

```c
#define CB2000_NR_ENROLL_STAGES 15
```

This is deliberate.

Each press captures only a small region of the finger. A low number of stages
may look simpler, but it tends to produce poor template coverage and a less
stable feature map. The result is usually one or more of:

- repeated low-diversity captures
- more `retry-scan` outcomes
- more `remove-and-retry` outcomes
- worse verify robustness
- more false `no-match` events on the correct finger

The current expectation is a lift-and-shift sequence:

- start near the fingertip
- move slightly between accepted captures
- avoid repeatedly presenting the exact same patch

The goal is not just "15 photos". The goal is enough distinct local coverage to
assemble a robust descriptor mosaic from a tiny sensor.

## Retry Semantics

Retries are not automatically evidence of failure. Some of them are expected and
reflect quality control.

Typical retry classes:

- `retry-scan`
  the current capture was not good enough and should be retried
- `remove-and-retry`
  the finger should be lifted and repositioned

In practice these come from a mix of:

- insufficient quality
- insufficient usable area
- unstable placement
- repeated sampling of nearly the same local region

The driver keeps diagnostics around retry causes because this is an important
part of tuning the device behavior.

## Thermal Override

The driver sets:

```c
dev_class->temp_hot_seconds = -1; /* Solves false temperature hot shutdown */
```

Reason:

- libfprint has a generic virtual temperature model
- on this device, that model can block longer enrollment sessions even when the
  stop condition is not reflecting a meaningful real-world overheating risk
- with a `15`-stage enrollment and natural retries, this was creating false
  software shutdowns during valid user interaction

So the driver disables that virtual threshold for the CB2000 to prevent false
`FP_TEMPERATURE_HOT` interruptions.

## Helper Sidecar

The OpenCV sidecar remains part of the active design. The main driver and the
helper should be understood as one runtime path:

- main driver in `src/canvasbio_cb2000.c`
- matcher core in `src/cb2000_sigfm_matcher.c`
- OpenCV helper built as `libcb2000_sigfm_opencv.so`

The helper supports the feature-mosaic path used by the current template model.

## Maturity Statement

The driver is functional and suitable for continued testing and iterative use,
but it is still being tuned. Public documentation should therefore describe it
as an active development driver rather than a finalized mass-distribution
package.
