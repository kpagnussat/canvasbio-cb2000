# 0007. Document implementation provenance

Status: accepted

## Context

Using a proprietary package as a behavioral reference
([0006](0006-observed-behavior-as-compatibility-target.md)) raises an
important provenance question: what information guides the implementation,
and what material is actually published? The answer should be explicit in a
project released under a free licence and intended for upstream review.

## Decision

All source in this repository is written independently for this project from
documented behavioral findings gathered for interoperability. It has its own
structure, names and implementation. It is not a translation of decompiler
output. No vendor source code, binary, decompiler output, table or data file
is copied or redistributed. The repository publishes the project's own code
and derived functional facts such as request codes, sequences, status bytes,
thresholds and algorithm structure.

## Consequences

- The implementation follows a documented behavioral specification rather
  than the expression of the vendor program.
- Numeric constants read out of the binary appear in the source as numbers
  with an explanation of what they gate, because a number that describes an
  interface is a fact, not an expression.
- Anyone wanting to repeat the differential comparison needs their own lawful
  copy of the vendor package. The method for doing that is published in full
  ([0011](0011-measure-behavioral-agreement.md)); the tools and dumps are
  not.
- `COPYRIGHT` states this, and every source file carries the project's own
  licence header.

## Evidence

`COPYRIGHT`, the file headers in `src/`, and the compatibility comments in
`src/cb2000_engine*`, which describe observed behavior rather than quote code.
