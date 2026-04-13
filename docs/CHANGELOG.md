# Changelog

This changelog keeps only the version deltas that still explain the current
public snapshot.

## Unreleased

- disabled libfprint's generic thermal cutoff for the CB2000 with
  `dev_class->temp_hot_seconds = -1`
- refreshed public docs to match the current code path:
  - `15` enrollment stages
  - helper-backed mosaic-first matching
  - gallery fallback retained
  - relaxed default enrollment gates

## R2.5

Current public snapshot.

- `CB2000_OPTIMIZED_MODE_DEFAULT=1` remains the default runtime profile
- verify keeps the blank-probe auto-retry path
- public docs now describe the actual hybrid matcher stack directly from the
  current code
- package and lab documentation stay aligned with the OpenCV-helper-dependent
  public snapshot

## R2.4

- introduced the current template packing with raw enrollment images plus
  mosaic keypoints
- added helper-backed mosaic build and mosaic match paths
- kept gallery fallback for inconclusive or unavailable mosaic cases

## R2.3

- standardized the driver on `15` enrollment stages
- enforced lift-and-shift with the finger-removal stage between accepted
  captures

## R2.2

- added duplicate-position handling so near-identical captures no longer waste
  enrollment slots

## R2.1

- relaxed default enrollment gating away from the more aggressive earlier
  behavior

Why this history is still kept:

- later documents refer to `R2.4` for the current mosaic-backed template path
- later documents refer to `R2.3` for the `15`-stage model
- later documents refer to `R2.5` as the current public snapshot
