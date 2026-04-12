# SIGFM & Goodix Driver Research

**Project location:** `/home/kris/Data/Estudo-Conhecimento/Projetos/CanvasMancer`

Date: 2026-02-25

## Sources

- [SIGFM repository](https://github.com/goodix-fp-linux-dev/sigfm) — original C++ reference
- Goodix MOC driver: `libfprint/drivers/goodixmoc/` (local copy in container)
- Our C port: `src/cb2000_sigfm_matcher.c`

---

## 1. SIGFM Original Algorithm (C++ reference)

SIGFM = "SIFT Is Good For Matching". Designed for **low-resolution sensors** (64x80).

### What SIGFM expects from the image

The original `compute.cpp` reveals the preprocessing SIGFM was designed for:

```cpp
cv::Mat image = clear - cv::imread(path, 0);       // 1. Background subtraction
minMaxLoc(image, &minimum, &maximum, NULL, NULL);
image = (255/(maximum-minimum)) * (image - minimum); // 2. Min-max normalization to 0-255
cv::SIFT::create()->detectAndCompute(image, ...);     // 3. SIFT on clean image
```

**Key finding**: SIGFM expects exactly TWO preprocessing steps:
1. **Background subtraction** — subtract a "clear" (no finger) frame
2. **Min-max contrast stretch** — linear normalization to full 0-255 range

**NO CLAHE. NO upscale. NO Gaussian blur. NO double normalization.**

The original uses OpenCV SIFT which internally computes gradients on the raw
normalized image. Our C port does the same gradient computation (Sobel-like
3x3 kernel in `cb2000_sigfm_compute_grad`).

### Match algorithm constants (C++ reference)

```cpp
DISTANCE_MATCH = 0.75   // Lowe's ratio test threshold
LENGTH_MATCH   = 0.05   // Vector length consistency tolerance
ANGLE_MATCH    = 0.05   // Angle consistency tolerance
MIN_MATCH      = 5      // Minimum matches for verification
```

### Our C port defaults vs original

| Parameter | Original C++ | Our C port | Match? |
|---|---|---|---|
| ratio_test | 0.75 | 0.75 | YES |
| length_match | 0.05 | 0.05 | YES |
| angle_match | 0.05 | 0.05 | YES |
| min_matches | 5 | 5 | YES |
| cell_w/cell_h | N/A (SIFT native) | 8x8 | N/A (different keypoint detector) |
| sigma_factor | N/A | 0.75 | N/A |
| min_peak | N/A | 24.0 | N/A |
| max_keypoints | N/A (SIFT native) | 40 | N/A |

Our C port replaces OpenCV SIFT with a simplified cell-based gradient keypoint
detector (no scale space, no octaves). The geometric verification (angle+length
consensus) is a faithful translation of the C++ original.

---

## 2. Goodix MOC Driver Analysis

The Goodix MOC driver is a **Match-on-Chip** design — fundamentally different
from our Match-on-Host approach. The firmware does all image processing and
matching internally.

### Key parameters

```c
#define DEFAULT_ENROLL_SAMPLES 8     // 8 enrollment captures

// sensor_config[26] defaults:
// config[3] = 0x50 (80) — max overlay ratio for enrollment diversity
// config[4] = 0x0f (15) — minimum image quality threshold
// config[5] = 0x41 (65) — minimum image coverage threshold (%)
// config[6] = 0x08 (8)  — number of enrollment stages
```

### Goodix enrollment quality gates

Two-stage quality check:

**Stage 1 — Verify capture (L419-430):**
- `img_quality == 0` → reject (remove finger)
- `img_coverage < 35` → reject (center finger)

**Stage 2 — Enrollment capture (L691-704):**
- `img_quality < config[4]` (default 15) → reject
- `img_coverage < config[5]` (default 65%) → reject

**Stage 3 — Enrollment update diversity (L723-728):**
- `img_preoverlay > config[3]` (default 80) → reject (too much overlap with previous samples)

### Goodix image preprocessing: NONE in the driver

The Goodix driver sends NO image data to the host for processing. The firmware
handles everything:
- Image capture
- Quality assessment (`img_quality`, `img_coverage`)
- Template generation
- Matching (verify returns a boolean `resp->verify.match`)

**The host driver is a pure command/response wrapper.** There is no
`normalize_contrast`, no CLAHE, no upscale, no SIGFM — the chip does it all.

---

## 3. Cross-Reference with Our CB2000 Pipeline

### What our pipeline does vs what SIGFM expects

| Step | SIGFM expects | Our pipeline (CB2000_UPSCALE=2) | Our pipeline (CB2000_UPSCALE=1) |
|---|---|---|---|
| Background sub | YES (`clear - image`) | YES (`apply_background_subtraction`) | YES |
| Contrast norm | YES (min-max 0-255) | YES but DOUBLE (CLAHE + norm + blur + norm) | YES but CLAHE + norm (single) |
| CLAHE | NO | YES (clip_limit=3.0, 8x8 tiles) | YES |
| Upscale roundtrip | NO | YES (2x up, blur, 2x down = low-pass filter) | NO |
| Gaussian blur | NO | YES (3x3 on upscaled image) | NO |
| Double normalize | NO | YES (re-normalizes after blur) | NO |

### Problem diagnosis

Our preprocessing chain adds THREE steps that SIGFM was never designed for:

1. **CLAHE** — redistributes local contrast. This changes gradient magnitudes
   that SIGFM uses for keypoint detection. May help or hurt depending on image.

2. **Upscale→blur→downsample** (when upscale=2) — effectively a low-pass filter
   that DESTROYS high-frequency ridge detail. SIGFM detects keypoints via
   gradient magnitude peaks. Blurring reduces gradients = fewer keypoints =
   lower scores.

3. **Double normalization** (when upscale=2) — re-stretches an already normalized
   histogram, potentially amplifying blur artifacts.

### What Goodix teaches us

Goodix solves quality at the SOURCE:
- **Firmware-level quality gate**: rejects bad captures before they enter the template
- **Coverage gate**: ensures finger covers >= 65% of sensor area
- **Diversity gate**: rejects samples with > 80% overlap (forces different positions)
- **8 samples**: more coverage of the fingerprint surface
- **NO host-side preprocessing**: the chip captures a clean image or rejects it

We should emulate this philosophy:
- Quality gate at enrollment (reject bad captures before storing)
- Coverage/area gate (already have `area_min_pct`)
- Increase to 8 samples
- Minimize preprocessing before SIGFM — let it work on the cleanest possible data

---

## 4. Recommendations

### Immediate (test now)

1. **Test with `CB2000_UPSCALE=1`** — removes the blur→downsample roundtrip and
   the double normalization. SIGFM gets: bg_subtract → CLAHE → normalize (1x).
   This is closer to what SIGFM was designed for.

### Short-term (next version)

2. **Add option to disable CLAHE for SIGFM path** — SIGFM was designed for
   simple min-max normalization, not adaptive histogram equalization. Test with
   `bg_subtract → normalize_contrast (min-max)` only.

3. **Strengthen enrollment quality gate** — emulate Goodix firmware:
   - Reject captures with variance below threshold (current gate exists)
   - Reject captures with coverage (foreground area) below 65%
   - Reject captures with too much overlap with previous samples (diversity
     gate exists but may need tighter thresholds)

4. **Increase enrollment to 8 samples** — matches Goodix default and Windows
   CB2000 driver behavior (40KB template = 8 × 5120 bytes).

### Medium-term

5. **Peak Match classifier** — compare probe against each of the 8 gallery
   samples independently, take the single best score, apply a strict threshold.
   Eliminates the consensus requirement that is geometrically impossible on a
   tiny sensor.

6. **Background frame capture** — capture a "clear" frame (no finger) during
   device activation. Use for proper SIGFM-style background subtraction.
   Currently `background_valid` may not always be set.