# 0009. Drop OpenCV, SIFT and the code derived from SIGFM

Status: accepted

## Context

The matcher that shipped in R2.5 used SIFT keypoints over a mosaic of
touches, built on OpenCV, with code adapted from the SIGFM project. It was
measured on the corpus at 63.9% genuine accepts across sessions and 2.9%
accepts between different fingers, and the mosaic itself accepted three
impostor touches against two genuine ones. Once the independent implementation
described in [0006](0006-observed-behavior-as-compatibility-target.md) was
complete, there were two matchers in the tree, one of them better on every
number.

OpenCV also cost the package a C++ runtime, a set of versioned library
dependencies that pinned the package to one distribution release, and a
runtime loaded helper with a silent fallback path.

## Decision

Remove the SIFT matcher, the mosaic and everything derived from SIGFM, and
with them the OpenCV dependency. The driver is plain C with GLib, and the
independent implementation is the only matcher.

## Consequences

- The package depends on `libfprint-2-tod1` and nothing else, which is what
  makes building it on other distributions realistic.
- Third party licence obligations disappear with the third party code. Every
  file in the repository is the project's own, under LGPL-2.1-or-later.
- Quality gates, diversity gates and the duplicate touch check went with the
  old matcher, because the vendor engine has none of them: it takes every
  touch and its refusals are decisions of its own.
- The old matcher is not in this repository's history. It is kept privately,
  and only as something to compare against.

## Evidence

`meson.build` (no OpenCV), `COPYRIGHT`, and the comparison table in
`docs/RESEARCH.md`, "Offline corpus".
