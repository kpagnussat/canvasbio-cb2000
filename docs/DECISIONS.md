# CB2000 Architecture Decisions

This file records the decisions that define the current public snapshot.

## D-01: Match on Host

**Decision**: keep the driver as match-on-host.

**Why**:

- the public protocol surface exposed to the driver is image capture oriented
- the driver stores and evaluates prints in host software through libfprint
- current verify/identify decisions are produced in the host process

## D-02: Feature Matching Instead of Minutiae Pipeline

**Decision**: use the custom SIGFM-oriented matcher stack instead of
libfprint/NBIS minutiae matching.

**Why**:

- the sensor is only `80x64`
- the active area is small enough that classic minutiae extraction is not the
  practical center of this implementation
- the current driver already owns template format, matching, and telemetry

## D-03: Keep the Hybrid Matcher Stack

**Decision**: keep the current hybrid stack instead of pretending the project
is already OpenCV-free.

**Current behavior**:

- the current template path tries the OpenCV mosaic matcher first
- pair matching prefers the OpenCV helper when available
- the in-tree C matcher remains as fallback

**Why**:

- this is what the public code actually does today
- the helper is still required for the full mosaic path
- documenting it any other way would be inaccurate

## D-04: Store Raw Gallery Images Plus Mosaic Data in the Current Template

**Decision**: keep the current template storing both raw enrollment images and mosaic
keypoints.

**Why**:

- the mosaic path is preferred, not guaranteed
- gallery fallback remains part of current behavior

## D-05: Default to the Optimized SIGFM-Clean Driver Path

**Decision**: keep `CB2000_OPTIMIZED_MODE_DEFAULT=1`.

**What that means**:

- the driver-side default path uses background subtraction plus min/max
  normalization to build the SIGFM-clean frame
- driver-side optimized preprocessing does not apply CLAHE
- helper-side operations still apply their own CLAHE plus normalization

**Why**:

- this matches the current code
- it keeps the in-driver path simpler while preserving the helper-side mosaic
  behavior already in use

## D-06: Keep 15 Enrollment Stages

**Decision**: keep `15` enrollment stages.

**Why**:

- the sensor is too small for stable single-view enrollment
- the current template model depends on multi-capture coverage
- `nr_enroll_stages` is explicitly wired into the device class

## D-07: Enforce Lift-and-Shift Between Accepted Stages

**Decision**: keep the finger-removal state as part of enrollment flow.

**Why**:

- consecutive captures without removal tend to collapse into duplicate local
  coverage
- current enrollment quality depends on distinct local regions

## D-08: Relax Default Gates but Keep Duplicate Rejection

**Decision**: ship the public snapshot with relaxed default enrollment gates,
while keeping duplicate-position handling active.

**Current state**:

- ridge-quality gate implemented, disabled by default
- diversity success gate implemented, disabled by default
- duplicate threshold handling still active

**Why**:

- the current code no longer behaves like an aggressively gated enrollment
  pipeline
- duplicate rejection is still technically useful because repeated placement
  wastes one of the `15` stages

## D-09: Keep the Blank-Probe Auto-Retry

**Decision**: allow one automatic verify retry for clearly empty/weak probes.

**Trigger**:

- verify result is `NO_MATCH`
- best consensus is `0`
- best score is below `0.05`
- auto-retry budget has not been used yet

**Why**:

- this reduces friction from obviously bad captures without turning repeated
  failures into hidden behavior

## D-10: Disable libfprint's Generic Thermal Cutoff for This Device

**Decision**: keep `dev_class->temp_hot_seconds = -1`.

**Why**:

- the public snapshot uses long multi-stage enrollment
- libfprint's generic thermal model was causing false hot shutdowns here
- the current code explicitly treats this as part of the intended runtime
  behavior
