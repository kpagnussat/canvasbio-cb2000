# Changelog

All notable changes to the CB2000 libfprint driver.

---

## Unreleased

### Changes
- **False hot-shutdown fix**: `dev_class->temp_hot_seconds = -1` in the CB2000
  device class init to disable libfprint's virtual thermal throttling for this
  USB sensor. This avoids false `FP_TEMPERATURE_HOT` aborts during long
  enrollment or retry-heavy sessions.
- **Operational docs refresh**: active documentation now reflects the current
  Ubuntu tester flow, USB permission helper, isolated lab roots under
  `~/.ContainerConfigs/<DBX_NAME>/`, and host-path install tips emitted by the
  distro package builders.

---

## R2.5 — CLAHE + Enrollment Guidance + Verify Retry
**Status**: Current release — GAR=100%, FAR=0%

### Changes
- **CLAHE preprocessing**: Applied before SIFT keypoint extraction (tile 8×8, clip 2.0)
  both at enrollment (mosaic build) and at verify (probe matching). This was the
  decisive improvement: GAR jumped from 94.1% to 100%.
- **Enrollment guidance**: Added `CYCLE_WAIT_REMOVAL` enforcement between all
  enrollment stages. The driver now waits for finger removal before proceeding to
  the next stage, ensuring coverage variation across the 15 captures.
- **Verify auto-retry**: One silent retry when the probe is detected as blank
  (`consensus=0` AND `score<0.05`). Avoids spurious NO MATCH from premature
  finger lifts.

---

## R2.4 — Feature Mosaicking (Two-Hop Affine)
**GAR**: ~94.1% (before CLAHE)

### Changes
- **Print v2 format**: New GVariant format `(uqq@ay@ay)` — packs 15 raw images
  plus a master mosaic keypoint set. v1 prints remain fully supported (fallback
  to gallery loop matching).
- **Mosaic build** (`cb2000_sigfm_opencv_build_mosaic`): Aligns all enrollment
  images to a common reference coordinate system using two-hop affine alignment
  (RANSAC thresh=3px, min_inliers=4). Deduplicates keypoints within 3px radius.
- **Mosaic match** (`cb2000_sigfm_opencv_match_mosaic`): SIFT on probe → BFMatcher
  ratio test → estimateAffine2D+RANSAC against mosaic descriptors. Score=1.0 if
  inliers≥min_matches, else inlier_ratio.
- **Mosaic max keypoints**: Increased from 400 to 600 after finding that the limit
  was too tight for the aggregated 15-image keypoint set.

---

## R2.3 — 15 Enrollment Stages
**GAR**: ~85%

### Changes
- Increased enrollment from 8 to 15 stages.
- Each stage requires a full lift-and-shift (finger down, capture, finger up).
- This provided the additional coverage variation needed for reliable mosaicking.

---

## R2.2 — Anti-Duplicate Gate + RANSAC Fix
**GAR**: ~75%

### Changes
- Added anti-duplicate gate to reject captures that are near-identical to a
  previously accepted enrollment image (prevents wasting stages on the same pose).
- Fixed RANSAC minimum from 3 to 4 inliers (3-point RANSAC is degenerate for
  affine transforms — the transform is determined exactly with no constraint).

---

## R2.1 — Quality Gate Disabled
**GAR**: ~88% (baseline without gate interference)

### Changes
- Disabled the enrollment quality gate (was filtering on variance/contrast
  thresholds that forced users to present outlier poses).
- Result: 0 retries, 88% GAR — confirming FIND-024.

---

## R2.0 — Clean Baseline (Gate Investigation)
**GAR**: ~41–88% depending on gate configuration

### Changes
- Reverted all gate changes to understand the gate↔GAR relationship.
- Empirical sessions confirmed FIND-024: inverse correlation between enrollment
  retries and verify GAR.
- `strict_low` set to 0 (FIND-025: low-link pairs on 80×64 are normal, not failures).

---

## A1.x Series — Quality Gate Experiments
**GAR**: 41–65% (degraded by gate tuning)

### Context
Attempted to improve GAR by tightening enrollment quality gates. Each tightening
increased retry count and degraded GAR. The series culminated in FIND-024 (gate
hypothesis proven empirically).

---

## B1 — First Functional Version
**GAR**: ~60% (variable, no systematic measurement)

### Changes
- First working driver: USB protocol reverse-engineered from pcap captures.
- Implemented SIGFM C port (derived from upstream C++ reference).
- Enrollment: 8 stages, basic gate configuration.
- Template: v1 format — single-image SIFT descriptor set per stage.
- Matching: gallery loop over all enrollment images, best score wins.
- Proved that SIFT on 80×64 is feasible. NBIS/bozorth3 discarded.

---
