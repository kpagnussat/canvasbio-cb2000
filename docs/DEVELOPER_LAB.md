# Developer Lab

This repository keeps a development lab on purpose.

It is meant for:

- developers who want to improve the driver
- tinkerers who want to rebuild, test and inspect behavior
- packagers who want distro-native artifacts
- contributors who need real runtime and packaging helpers

It is not framed as a polished consumer installer.

## Scope

The lab covers:

- integration into a local `libfprint` tree
- sidecar helper builds
- runtime session helpers
- USB permission helpers
- report generation
- distro package generation

The scripts are there to reduce friction for technical contributors, not to
promise a universal one-command install experience for all users.

## Public Boundary

Some internal workflow remains intentionally out of scope for the public
repository narrative.

Examples of things that should not be central in public-facing docs:

- local RAG workflow
- personal indexing/bootstrap routines
- private iteration habits
- deeply personal project-management structure

Those can exist locally, but they should not define the public explanation of
the repository.

## Where To Start

- overall script catalog:
  `scripts/README.md`
- driver rationale:
  `docs/DRIVER_NOTES.md`
- package download and install guidance:
  `releases/README.md`

## Status Label For Scripts

The correct framing for the `scripts/` tree is:

- developer lab
- operational helpers
- packaging helpers

Not:

- official end-user installer
- guaranteed zero-friction production deployment tool
