# 0010. A print holds the engine's template, not captured frames

Status: accepted

## Context

While the matcher was SIFT over a mosaic, the print stored for each enrolled
finger held captured frames, because the mosaic was rebuilt from them. That
means grayscale pictures of a fingerprint in `/var/lib/fprint/`. The
independent matcher uses a template format of its own: a small directory plus
node records, each node holding two bit planes (ridges and mask), a feature
score, the minutiae and the keypoints, and no pixel buffer at all.

## Decision

A print holds the engine's template buffer, inside a versioned GVariant that
also records the frame width and height. Nothing else is stored. A print
whose version this driver does not know, or whose frame size is not this
sensor's, is never handed to the engine.

## Consequences

- No grayscale image is stored anywhere by default
  ([0002](0002-no-images-on-disk.md) covers the debug dump).
- The binarized ridge pattern is still a picture of the fingerprint in every
  meaningful sense, and the README says so rather than claiming the print is
  anonymous. It cannot be removed without removing the matcher, because it
  *is* the vendor's template.
- Prints enrolled with R2.5 cannot be read. They end verify with a "data
  invalid" error instead of asking for another touch forever, and identify
  skips them and still compares the others. Users must delete and enroll
  again, which the README and `CHANGELOG.md` state up front.
- The template parser treats a stored print as untrusted input: a malformed
  or truncated buffer is refused, and the node count is capped.

## Evidence

`src/cb2000_print.{c,h}`, the format contract in `src/cb2000_engine.h`, the
encode and decode path in `src/cb2000_engine_compare.c`, and the malformed
input checks in `tests/offline/cb2000-engine-test.c`.
