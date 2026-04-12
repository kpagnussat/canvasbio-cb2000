# CB2000 Architecture Decisions

Technical decisions and their justifications for the CB2000 libfprint driver.

---

## D-01: Match-on-Host vs Match-on-Device

**Decision**: Implement as Match-on-Host (MoH).

**Context**: The CB2000 firmware exposes only raw image capture. No enrollment
or match commands exist in the USB protocol. The device has no on-board
template storage accessible to the driver.

**Consequence**: The driver implements enrollment (template building) and
matching (score computation) entirely in the host process using libfprint's
`FpDevice` APIs with custom match callbacks.

---

## D-02: SIFT over Minutiae (NBIS/bozorth3)

**Decision**: Use SIFT feature matching instead of minutiae-based matching.

**Rationale**:
- Sensor: 80×64 px, 340 DPI → active area 6.0 × 4.8 mm
- Ridge spacing at 340 DPI ≈ 4–5 pixels
- Reliable minutia extraction requires ≥500 DPI and larger sensor area
- NBIS `mindtct` produces unreliable minutiae at this resolution
- `bozorth3` matching with <20 reliable minutiae is fundamentally broken
- SIFT operates on texture gradients — works adequately on 80×64

**Validation**: NBIS approach tested in B1 and discarded. SIFT (via SIGFM)
adopted in A3 and retained through R2.5.

---

## D-03: SIGFM as SIFT Matching Library

**Decision**: Use SIGFM (C implementation derived from upstream C++ reference).

**Rationale**: SIGFM provides a fingerprint-specific SIFT pipeline with
ratio test, length/angle filtering, and consensus voting. It handles the
fingerprint-specific matching heuristics that raw OpenCV SIFT lacks.

The C implementation (`cb2000_sigfm_matcher.c`) was derived from the upstream
C++ reference and verified for parameter parity.

---

## D-04: Feature Mosaicking (Two-Hop Affine)

**Decision**: Build a mosaic of all 15 enrollment images aligned to a common
reference coordinate system (two-hop affine alignment).

**Why not star topology**: Gallery image 0 as reference works only if it has
significant overlap with all other images. On 80×64 with lift-and-shift
enrollment, some pairs have <4 RANSAC inliers when aligned directly.
Star topology collapses when the hub image is a partial capture.

**Two-hop solution**: For each image `i`, find the most similar intermediate
image `j` (highest SIGFM link score), align `i→j→0`. This reduces the
alignment burden per step and avoids degenerate cases.

**Error bound**: 2 hops × 3 px RANSAC threshold = ≤6 px accumulated error.
Acceptable given SIFT keypoint localization precision (~1 px).

---

## D-05: CLAHE Preprocessing

**Decision**: Apply CLAHE (Contrast Limited Adaptive Histogram Equalization)
before SIFT keypoint extraction, both at enrollment and verify.

**Parameters**:
- Tile size: 8×8 pixels (matches C CLAHE grid used in the image pipeline)
- Clip limit: 2.0

**Rationale**: Raw 80×64 images after background subtraction have
non-uniform contrast. Low-contrast regions yield few SIFT keypoints.
CLAHE locally enhances contrast, allowing SIFT to find keypoints across
the full image, not just high-contrast ridge areas.

**Evidence**: GAR improved from 94.1% (R2.4) to 100% (R2.5) in controlled
testing (17/17 successful verifications).

---

## D-06: 15 Enrollment Stages

**Decision**: Use 15 enrollment stages with mandatory lift-and-shift between
stages.

**Why 15**: Balances coverage (more images = better mosaic = higher GAR) with
UX (each stage requires a lift-and-shift). Tested configurations: 8 stages
gave GAR ~70%, 12 stages ~88%, 15 stages ~94–100%.

**Lift-and-shift requirement (`CYCLE_WAIT_REMOVAL`)**: The sensor cannot
distinguish a re-placement from a held finger. Without waiting for finger
removal, consecutive captures are nearly identical (no coverage variation).
This is enforced between every enrollment stage.

---

## D-07: Verify Auto-Retry

**Decision**: Allow one automatic retry when the probe is blank (empty image).

**Trigger**: Blank probe detected when `consensus=0` AND `score<0.05`.

**Rationale**: Users occasionally lift their finger before the image is
captured, producing a blank frame. One silent retry avoids a spurious
NO MATCH result without hiding real failures.

**Limit**: Maximum 1 retry to avoid masking repeated placement failures.

---

## D-08: Disable Enrollment Quality Gate

**Decision**: Quality gate disabled in R2.0+ based on FIND-024.

**Evidence**: Empirical testing showed inverse correlation between gate
retries and verify GAR. 0 retries (gates OFF) → 88% GAR; 66 retries
(gates ON) → 41% GAR.

**Retained**: Precheck floor (min_var=3000, min_focus=450) as a garbage
filter to reject completely blank or smeared images. This floor is not
raised — it catches hardware failures, not marginal quality variation.

---

## D-09: FpDevice with Custom Matching

**Decision**: Implement as `FpDevice` (libfprint's generic device type)
with custom enrollment and match callbacks, rather than using libfprint's
built-in minutiae pipeline.

**Rationale**: libfprint's built-in pipeline assumes standard fingerprint
image quality and uses its own NBIS-based matching. On 80×64, this produces
unreliable results. `FpDevice` with custom callbacks gives full control over
the template format, matching algorithm, and score computation.

**Requirement**: `fpi_device_class_auto_initialize_features(dev_class)` must
be called in the class init function, or libfprint will not expose the device
features correctly.

---
