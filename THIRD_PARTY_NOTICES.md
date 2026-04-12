# Third-Party Notices

## SIGFM (algorithmic reference)

- Component: SIGFM reference used for matcher parity work.
- Upstream repository: `https://github.com/goodix-fp-linux-dev/sigfm`
- Upstream commit: `dc591ca03276348ad731ab78c8d722fa40b3158f`
- Copyright: Goodix Fingerprint Linux Development
- License: MIT

Portions of matcher behavior and validation helper logic in:
- `src/cb2000_sigfm_matcher.c`
- `src/cb2000_sigfm_opencv_helper.cpp`

are derived/adapted from the SIGFM upstream reference.

## OpenCV (current required runtime/build dependency)

- Component: OpenCV, used by the active helper `libcb2000_sigfm_opencv.so`.
- Scope: current `R2.5` feature-mosaic enrollment/verify path. For this public
  snapshot, that makes OpenCV a required build and runtime dependency rather
  than an optional extra.
- License: OpenCV license (provided by your OpenCV distribution).
