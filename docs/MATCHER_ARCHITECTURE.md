# CB2000 Matcher Architecture

This document describes the current matcher flow implemented by the public
snapshot. Source of truth: `src/canvasbio_cb2000.c`,
`src/cb2000_sigfm_matcher.c`, and `src/cb2000_sigfm_opencv_helper.cpp`.

## Hardware Baseline

- Sensor frame: `80x64` grayscale (`5120` bytes)
- Resolution: about `340 dpi`
- Active area: `6.0 mm x 4.8 mm`

The sensor is small enough that template quality depends heavily on multi-stage
coverage, not on a single capture.

## Current Runtime Model

The public `R2.5` snapshot uses a hybrid matcher stack:

1. The driver captures a raw `80x64` frame.
2. If a background frame is available and background subtraction is enabled,
   the driver subtracts it before matching preparation.
3. With the current default `CB2000_OPTIMIZED_MODE_DEFAULT=1`, the driver builds
   a SIGFM-clean frame using background subtraction plus min/max normalization.
   This in-driver path does not apply CLAHE.
4. `unpack_and_match()` first tries the OpenCV mosaic helper.
5. If the mosaic path is unavailable or inconclusive, the driver falls back to
   per-gallery matching against the stored enrollment images.
6. Per-gallery scoring uses `cb2000_sigfm_pair_score_transposed()`, which
   prefers the OpenCV pair helper when available and otherwise falls back to the
   in-tree C implementation.

## Template Model

The current template stores:

- the `15` raw enrollment images
- the helper-built mosaic keypoint set

Packing behavior is:

- try `cb2000_sigfm_opencv_build_mosaic()`
- if mosaic build succeeds, pack the current mosaic-backed template
- if it fails or the helper is unavailable, pack the gallery-only fallback

## Verify and Identify Flow

The current decision flow is:

1. Try mosaic match through `cb2000_mosaic_match_fn`
2. Accept the mosaic result only when it is strong enough
3. Otherwise fall back to the gallery loop
4. Mark `MATCH` when at least one comparison reports `original_match`

Important thresholds in the current public snapshot:

- pair matcher defaults:
  - `ratio_test = 0.75`
  - `length_match = 0.15`
  - `angle_match = 0.05`
  - `min_matches = 3`
- mosaic acceptance guard:
  - raw matches must be at least `5`
- gallery fallback guard:
  - consensus minimum is raised to `4` when the mosaic-backed template path is
    active

These thresholds are intentionally split between pair matching, mosaic
acceptance, and gallery fallback.

## OpenCV Helper Role

The OpenCV helper remains part of the active design:

- pair helper symbol: `cb2000_sigfm_opencv_pair_match`
- mosaic builder: `cb2000_sigfm_opencv_build_mosaic`
- mosaic matcher: `cb2000_sigfm_opencv_match_mosaic`
- shared object name: `libcb2000_sigfm_opencv.so`

The helper performs its own preprocessing with CLAHE (`2.0`, `8x8`) plus
min/max normalization before SIFT extraction. This is separate from the
driver-side optimized SIGFM frame path.

## Enrollment Controls

Enrollment control is more nuanced than a simple "gates on/off" summary.

Current default behavior:

- ridge-quality gate exists in code but is disabled by default
- diversity success-ratio gate exists in code but is disabled by default
- the broader diversity reject block is currently compiled out
- anti-duplicate protection remains active

What is still actively enforced:

- `15` enrollment stages
- mandatory lift-and-shift via the finger-removal state
- duplicate-position replace-or-reject path when NCC link reaches the duplicate
  threshold (`0.85`)

## Runtime Telemetry

Useful telemetry emitted by the current code:

- `[SIGFM_MOSAIC]` for mosaic attempt and fallback conditions
- `[SIGFM]` for gallery-loop scores and aggregates
- `[SIGFM_PEAK]` for peak per-gallery evidence
- `[SIGFM_MATCH]` for final verify decision
- `[VERIFY_GATE]` for finalize-ACK routing
- `[VERIFY_READY_MATRIX]` for ready-query routing
- `[ENROLL_QUALITY]` and `[ENROLL_DIVERSITY]` for enrollment diagnostics

## Practical Summary

The correct high-level description of the current public snapshot is:

- match-on-host
- `15`-stage enrollment
- mosaic-first verify path
- gallery fallback retained
- OpenCV helper still required for the full mosaic path
- in-tree C matcher retained as pair-match fallback
