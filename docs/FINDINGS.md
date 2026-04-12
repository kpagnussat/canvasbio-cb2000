# CB2000 Technical Findings

Curated technical findings from reverse engineering, testing, and analysis
of the CanvasBio CB2000 fingerprint sensor.

---

## 1. Hardware e USB

### Sensor
- **Image size**: 80×64 pixels, grayscale (5120 bytes per frame)
- **Resolution**: 340 DPI
- **Active area**: 6.0 mm × 4.8 mm
- **VID/PID**: 0x2DF0 / 0x0003 or 0x0007

### Protocol
The CB2000 uses a simple request/response USB protocol over vendor-class transfers:

| Function | USB Command |
|----------|-------------|
| SetMode FINGER_DOWN | `REQ_INIT val=0x0007 idx=0` |
| WaitFingerDown | `REQ_POLL val=0x0007` — loop until byte[1] != 0 |
| WaitFingerUp | `REQ_POLL val=0x0007` — loop until byte[1] == 0 |
| FactoryReset | `REQ_INIT val=0x0001 idx=1` |
| Deactivate | `REQ_INIT val=0x0001 idx=0` |

### Capture Cycle
1. Send SetMode FINGER_DOWN
2. Poll until finger detected
3. Capture image (bulk read, 5120 bytes)
4. Poll until finger removed
5. Send Deactivate (partial — do NOT do full re-activation between enrollment stages)

**Critical**: Full re-activation (99 commands) between enrollment stages breaks
enrollment. Only send Deactivate (`REQ_INIT val=0x0001 idx=0`) between stages.

### Background Subtraction
Raw images contain background noise. Background subtraction is mandatory:
capture a reference image with no finger present, then subtract per-pixel.
Without it, SIFT finds keypoints on sensor artifacts rather than ridge patterns.

---

## 2. SIGFM and Matching

### SIGFM Algorithm
SIGFM (Scale-Invariant Feature-based Fingerprint Matching) is used for
match-on-host. It applies SIFT keypoint detection and matching with:

- **ratio test**: 0.75 (Lowe's ratio)
- **length tolerance**: 0.05 (accept if lengths differ by ≤5%)
- **angle tolerance**: 0.05 rad (≈2.9°)
- **min_matches**: 5 (minimum consensus pairs to accept)

### Why SIFT on 80×64
NBIS/bozorth3 (minutiae-based) requires a minimum sensor area to extract
reliable minutiae. On 80×64 px / 340 DPI, the ridges are ≈4–5 px apart —
too coarse for reliable minutia extraction. SIFT works on texture patterns
and performs adequately at this resolution.

### RANSAC Behavior
Using `min_matches=3` for `estimateAffine2D` leads to degenerate solutions
(3 points define the affine transform exactly, no geometric constraint from
consensus). The correct minimum is `min_matches≥4` for meaningful RANSAC.

### Matching Parameters (current, aligned to upstream match.cpp)
```
ratio=0.75, length=0.05, angle=0.05, min_match=5
```
Note: upstream demo.cpp uses `length_match=0.95` which encodes the same
±5% condition from the opposite direction. Not a discrepancy.

### Upstream SIGFM Quality Gate
Upstream SIGFM has **no explicit variance/contrast gate** — quality filtering
is implicit: low-quality images produce fewer keypoints, which falls below
`MIN_MATCH` → reject. Our `precheck` (min_var=3000, min_focus=500) is the
correct "garbage filter" floor. Do not raise these values.

---

## 3. Enrollment Gates

### FIND-024: Enrollment Gates are Counterproductive
**Finding**: Perfect inverse correlation between enrollment retries and verify GAR.

| Retries | GAR |
|---------|-----|
| 0 (gates OFF) | 88% |
| 66 (gates ON) | 41% |

**Root cause**: Tight gates force the user to present outlier poses to pass
the gate. These outlier samples degrade template quality because they are
not representative of normal finger placement. The template then mismatches
normal verify probes.

**Resolution (R2.0+)**: Quality gate disabled. Precheck floor kept as
garbage filter only.

Best sessions (20260226_170007, 20260226_184115): strict_low=0, 0 retries,
88% GAR with v1 matching.

### FIND-025: Diversity Gate `overlap` Field Misunderstood
**Finding**: `overlap` in `[ IMAGE ]` and `[ ENROLL_DIVERSITY ]` log lines
measures **pixel coverage** (how much of the sensor area the finger covers),
NOT image-to-image similarity.

- overlap=99 means "finger covers 99% of sensor" — normal on 80×64, not a problem
- Actual diversity is measured by SIGFM `link` scores

**SIGFM link scores for valid same-finger pairs on 80×64**:
- Typical range: 0.05–0.29
- Island captures (partial overlap): 0.05–0.15 — NORMAL
- `strict_low=1` counts low-link pairs as FAILURES → causes retry loops
- `strict_low=0`: low-link pairs are skipped (not counted as success or failure)

### Current Gate Settings (R2.5)
| Gate | Value | Notes |
|------|-------|-------|
| quality_gate | DISABLED | FIND-024: causes regression |
| precheck min_var | 3000 | garbage filter floor |
| precheck min_focus | 450 | garbage filter floor |
| diversity overlap_max | 98 | forces placement variation |
| diversity link_min | 0.080 | aligned to best session |
| diversity link_max | 0.480 | unchanged |
| diversity strict_low | 0 | CRITICAL: 1 causes retry loops |

---

## 4. Feature Mosaicking

### Why Mosaicking
15 enrollment images contain complementary ridge information. A single-image
template loses this. Mosaicking aggregates keypoints from all images into a
unified coordinate space, giving the matcher more reference points.

### Star Topology (R2.4 initial) and Its Limitation
Original approach: gallery[0] as reference, align all others to it.
Problem: if gallery[0] is a partial/island capture, alignment of images
with non-overlapping content fails (estimateAffine2D returns garbage
with <4 inliers).

### Two-Hop Alignment (R2.4 final)
To align image `i` to reference `0`:
1. Find the image `j` most similar to `i` (highest link score with `i`)
2. Align `i→j` then `j→0` (two affine transforms, composed)

Maximum accumulated error: ≤6 px over 2 hops of 3 px RANSAC threshold.
This is acceptable for SIFT matching (keypoint localization ≈1 px).

### CLAHE Preprocessing (R2.5)
Contrast Limited Adaptive Histogram Equalization applied before SIFT detection:
- Tile size: 8×8 (same grid as the C CLAHE used in image pipeline)
- Clip limit: 2.0
- Applied to: enrollment images before keypoint extraction (mosaic build)
  and probe images before matching

Effect: GAR 94.1% → 100% in controlled testing (17/17 verify attempts).
CLAHE enhances ridge contrast in low-contrast regions, enabling SIFT to
find keypoints that would otherwise be missed.

---
