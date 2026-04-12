# CB2000 Matcher Architecture

Updated to match current driver behavior in `src/canvasbio_cb2000.c`.

## Hardware baseline

- Sensor frame: `80x64` (5120 bytes)
- Resolution: `340 dpi`
- Active area: `6.0 mm x 4.8 mm`

## Current matcher model

The verify/identify decision path is now **SIGFM-only**.

- Core matcher: `src/cb2000_sigfm_matcher.c`
- Driver integration entry: `unpack_and_match()`
- Per-gallery score call: `cb2000_sigfm_pair_score_transposed()`
- Decision rule: `MATCH` if any gallery comparison sets `original_match`; otherwise `NO_MATCH`

There is no NCC-based classifier in verify/identify anymore.

## Pipeline summary

1. Capture raw `80x64` image from device.
2. Build canonical `sigfm_frame` for enroll/verify (no extra helper dependency).
3. For verify/identify, compare probe against all enrolled samples with SIGFM.
4. Emit decision telemetry (`[SIGFM_MATCH]`) and report to libfprint.

## Enroll gates (kept active)

Two gates are still active before accepting an enroll stage:

1. Quality gate (ridge telemetry)
- Minimum ridge count
- Minimum ridge spread
- Minimum ridge peak

2. Diversity gate (anti-duplicate)
- Uses masked normalized correlation helper `cb2000_ncc_match_u8_masked()`
- Purpose is enroll sample diversity only (not verify decision)

## Verify routing and USB parity

USB command sequence parity and verify routing states were preserved.

- `CYCLE_VERIFY_RESULT_STATUS` still classifies finalize ACK bytes.
- READY matrix (`VERIFY_READY_QUERY_A/B`) still controls retry/no-match short-circuit routing.
- Host decision path after routing is SIGFM-only.

## Runtime telemetry keys

- `[SIGFM]` per-gallery scores and aggregate top metrics
- `[SIGFM_PEAK]` best per-gallery peak candidate
- `[SIGFM_MATCH]` final verify/identify decision line
- `[VERIFY_GATE]` and `[VERIFY_READY_MATRIX]` routing telemetry
