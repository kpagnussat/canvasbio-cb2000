# 0002. Never write fingerprint images to disk by default

Status: accepted

## Context

Earlier builds wrote captured frames to disk to make debugging possible, and
kept captured frames inside the stored print. Both are readable pictures of a
fingerprint sitting in a file, one of them in a place the user never looks.

## Decision

The driver writes no image anywhere unless a developer opts in by setting
`CB2000_DEBUG_IMAGE_DIR`. Without that variable nothing is written. With it,
the directory is created with mode 0700, and the README states plainly that
the files are fingerprint images and should be deleted afterwards.

## Consequences

- Diagnosing a capture problem from a user's machine needs that user to opt
  in deliberately, which is the correct trade.
- The corpus used for measurement is collected with the same opt-in dump and
  lives outside this repository, in an encrypted volume.
- No biometric data of any kind is in this repository or in any release.

## Evidence

`src/canvasbio_cb2000.c`, the dump guarded by the variable. The print format
decision is [0010](0010-print-holds-the-template.md).
