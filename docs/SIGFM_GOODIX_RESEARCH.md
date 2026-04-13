# SIGFM and Goodix Research Notes

This file keeps only the research points that still matter for understanding the
current public snapshot.

## Scope

Two reference lines were studied during development:

- upstream SIGFM, as an algorithmic reference
- Goodix MOC drivers, as a contrasting fingerprint-driver architecture

Neither one should be read as a literal description of the current CB2000
driver. The CB2000 public snapshot has its own current behavior, documented in
the code and in `MATCHER_ARCHITECTURE.md`.

## 1. What Still Matters From SIGFM

Stable reference points that still help explain the current code:

- ratio-test baseline: `0.75`
- angle consistency baseline: `0.05`
- helper-backed SIFT path is still part of the active matcher stack

Important divergence from the old "strict parity" reading:

- current public defaults use `length_match = 0.15`, not `0.05`
- current pair matcher default is `min_matches = 3`, not `5`
- current v2 mosaic accept/fallback logic adds extra guards on top of pair
  matcher defaults

Why that divergence is retained:

- the public driver is tuned for an `80x64` sensor
- the current code intentionally separates pair defaults from mosaic acceptance
  guards

## 2. What Goodix Still Teaches Usefully

Goodix MOC remains useful only as a contrast model:

- it shows a firmware-heavy, match-on-chip architecture
- it reinforces the value of enrollment diversity and capture quality control

What does **not** carry over directly:

- Goodix sample counts
- Goodix firmware thresholds
- Goodix on-chip decision behavior

Reason:

- the CB2000 public snapshot is host-driven, not match-on-chip

## 3. Current CB2000 Takeaways

The current public code supports these concrete conclusions:

- `15` enrollment stages are intentional
- the helper-backed mosaic path is still active
- the driver-side optimized SIGFM path is still active
- the project is not yet in an OpenCV-free state

## 4. What Was Explicitly Superseded

The older research notes often pushed two ideas that are no longer the right
headline for the public snapshot:

1. "Increase enrollment to 8 samples"

Reason it was superseded:

- the public driver now explicitly uses `15` stages

2. "Document the project as if SIGFM parity implied a pure current path"

Reason it was superseded:

- the current matcher is hybrid: helper-backed mosaic first, gallery fallback
  second

3. "Treat Goodix defaults as if they were the CB2000 target configuration"

Reason it was superseded:

- Goodix MOC is useful as contrast, not as the current CB2000 source of truth

## 5. Practical Reading Rule

Use this file only for:

- algorithmic context
- architectural contrast
- understanding why the project ended up with a hybrid matcher stack

Do not use it as the normative description of current thresholds or runtime
flow. For that, read the current code, `FINDINGS.md`, and
`MATCHER_ARCHITECTURE.md`.
