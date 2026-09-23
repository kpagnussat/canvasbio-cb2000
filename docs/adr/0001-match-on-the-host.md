# 0001. Match on the host instead of with libfprint's minutiae matcher

Status: accepted

## Context

The CB2000 returns 80x64 grayscale frames, each covering an active area of
about 6.0 x 4.8 mm. The capture plan may read several frames for a touch, but
each frame still sees only that small window. The sensor never judges a touch
itself: it captures, and that is all it does. libfprint's own matching path
for image devices is NBIS, the NIST minutiae package, which needs enough ridge
endings and bifurcations spread over enough area to form a stable
constellation. A frame this small yields a handful of minutiae, sometimes
three, and upscaling it does not create more.

## Decision

The driver matches by itself. It is an image device in libfprint's sense for
capture, but verification, identification and enrollment are decided by code
in this repository, and the print it stores is that code's template.

## Consequences

- The project owns a matcher, which is a much larger commitment than a
  capture driver, and every accuracy question lands here rather than
  upstream.
- The matching code is kept free of libfprint and of USB, on plain buffers,
  so the offline tools can run exactly what the driver runs. See
  [0004](0004-measure-with-a-corpus.md).
- Upstreaming becomes an open question: whether libfprint wants an image
  driver that carries its own engine is not settled
  (`docs/RESEARCH.md`, "Open questions").

## Evidence

Minutiae counts on real frames are in `docs/RESEARCH.md`, "Why not NBIS".
Sensor geometry is in `docs/PROTOCOL.md`.
