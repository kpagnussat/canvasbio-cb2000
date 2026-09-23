# 0011. Measure behavioral agreement with differential testing

Status: accepted

## Context

The compatibility target defined in
[0006](0006-observed-behavior-as-compatibility-target.md) must be measured
rather than assumed. That requires the vendor engine in the test loop: feed
both implementations the same inputs and compare every reachable output
field.

## Decision

Behavioral agreement is measured against the vendor DLL, not against
expectations. A program compiled for Windows loads the vendor engine, reads a
binary file of real touches from the corpus, and records every output field it
can reach; the Linux implementation writes the same layout; a third program
compares them field by field and stops at the first divergence. Three levels
are used: the extractor stages one at a time, the engine's own API, and the
adapter entry points the Windows driver calls, with the hardware calls hooked
so no device is needed.

The method and measured results are published. The test programs, dumps,
extracted tables and biometric corpus stay private.

## Consequences

- The measured agreement comes with volumes and one numerical limit:
  templates identical byte for byte, every verdict identical, and 829 of
  14528 floating point values in the pairing transform differing in the last
  bit because the two C libraries do not round `atan2`, `cos` and `sin`
  identically.
- The limits of the comparison are stated with it: it does not establish
  accuracy, validate the USB side or cover input shapes absent from the
  corpus.
- A third party can recreate the method with a lawful copy of the vendor
  package and independent inputs. The exact runs cannot be distributed: the
  vendor binary is not ours to redistribute, and the corpus is one person's
  biometric data.
- Divergences are resolved in the independent implementation, never hidden by
  changing the compatibility target.

## Evidence

`docs/RESEARCH.md`, "Differential comparison": the three levels, volumes,
tables, floating point limit and its cause.
