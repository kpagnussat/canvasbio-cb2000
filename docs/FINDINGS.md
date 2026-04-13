# CB2000 Technical Findings

This document keeps only findings that are still useful for understanding the
current public snapshot.

## 1. Hardware and Transport

- Sensor frame: `80x64` grayscale (`5120` bytes)
- Effective resolution: about `340 dpi`
- Active area: `6.0 mm x 4.8 mm`
- Known USB IDs: `0x2DF0:0x0003`, `0x2DF0:0x0007`

The device is physically small enough that coverage sampling across multiple
captures is necessary for stable enrollment.

## 2. Current Matching Stack

The active stack is hybrid, not a single-path matcher:

- the current template path tries the OpenCV mosaic matcher first
- if the mosaic path is unavailable or inconclusive, the driver falls back to
  gallery matching against the stored enrollment images
- pair matching prefers the OpenCV helper when available and otherwise falls
  back to the in-tree C implementation

Current defaults visible in the code:

| Setting | Current value | Where it matters |
|---------|---------------|------------------|
| `ratio_test` | `0.75` | pair and helper paths |
| `length_match` | `0.15` | pair and helper paths |
| `angle_match` | `0.05` | pair and helper paths |
| pair `min_matches` | `3` | pair matcher default |
| mosaic raw-accept guard | `5` | mosaic accept/fallback decision |
| gallery fallback consensus min | `4` | stricter fallback when mosaic data is active |

Important implication:

- the current public snapshot uses helper-backed mosaic matching plus fallback
  pair matching
- thresholding is split between pair defaults, mosaic acceptance, and
  gallery fallback

## 3. Current Preprocessing Model

There are two preprocessing layers in the current codebase.

Driver-side default path:

- `CB2000_OPTIMIZED_MODE_DEFAULT=1`
- build SIGFM-clean frame from background subtraction plus min/max
  normalization
- no CLAHE in this in-driver optimized path

Helper-side path:

- the OpenCV helper applies CLAHE (`2.0`, `8x8`) plus min/max normalization
  before SIFT extraction
- this affects pair-helper and mosaic-helper operations

So the correct statement is not "CLAHE is always the active preprocessing path"
and not "CLAHE is gone everywhere". The current snapshot uses both layers in
different parts of the stack.

## 4. Enrollment Controls That Actually Matter Now

The current snapshot keeps some mechanisms implemented but not all of them are
active by default.

### Default gate state

| Control | Current default | Notes |
|---------|-----------------|-------|
| ridge-quality gate | `0` | implemented, disabled by default |
| diversity success gate | `0` | implemented, disabled by default |
| diversity `link_min` | `0.06` | current runtime default |
| diversity `link_max` | `0.48` | with optimized mode enabled |
| diversity `strict_low` | `0` | low-link pairs are not hard failures |
| diversity `overlap_max` | `98` | anti-duplicate upper bound |
| duplicate threshold | `0.85` | replace-or-reject trigger |
| precheck `min_var` | `3000.0` | garbage filter floor |
| precheck `min_focus` | `500.0` | garbage filter floor |

### Effective behavior

- the ridge-quality gate code exists, but the public snapshot ships with it
  disabled by default
- the diversity success-ratio gate code exists, but it is also disabled by
  default
- the broader diversity reject block is currently not active
- duplicate-position protection remains active and is the main enforced
  enrollment similarity control

This means current enrollment behavior is best understood as:

- relaxed default gates
- duplicate rejection still enforced
- `15` stages plus lift-and-shift still required

## 5. Template Model

The template model currently used by the driver is:

- `15` raw enrollment captures
- mosaic-backed template packing when mosaic build succeeds
- gallery-only fallback when mosaic build is unavailable or empty

The current template keeps both:

- raw gallery images
- aggregated mosaic keypoints

That retained raw gallery is what makes the current mosaic-first, gallery-second
fallback model possible.

## 6. Thermal Behavior

The device class sets:

```c
dev_class->temp_hot_seconds = -1;
```

This disables libfprint's generic thermal throttling model for this device and
avoids false `FP_TEMPERATURE_HOT` aborts during longer enroll sessions.

## 7. Practical Consequences

The main technically accurate takeaways for the public snapshot are:

- `15` enrollment stages are intentional, not leftover tuning noise
- the OpenCV helper is still a real runtime dependency
- mosaic matching is preferred, but gallery fallback remains part of the
  active behavior
- default enrollment gates are relaxed, while duplicate-position rejection is
  still active
- the thermal override is part of the intended runtime behavior
