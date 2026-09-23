# 0006. Use observed behavior as the compatibility target

Status: accepted

## Context

Four hand built matchers had been tried: NBIS, a normalized cross correlation
hybrid, SIFT keypoints, and SIFT over a mosaic of several touches. Each one
needed thresholds, and each threshold interacted with the others, so a bar
set on one set of templates let impostors through on another. Analysis of the
vendor's Windows package (CanvasBio WBF 4.1.14.882) documented an ordinary
but thoroughly tuned pipeline, with every gate carrying a constant and those
constants only meaning anything together.

The vendor tuned that pipeline against a corpus this project will never have,
and shipped it on a large number of laptops.

## Decision

Use the observed matching behavior as the compatibility target: implement the
same functional stages in the same order, with the documented constants,
rounding rules and result-affecting quirks. Do not recalibrate one number in
isolation or invent an alternative while observed behavior exists for the
same situation.

## Consequences

- The independently written implementation covers the extractor, pairing,
  score, decision table, enrollment and the messages the engine sends its
  driver rather than approximating them with local rules.
- Agreement is only meaningful once the whole chain is complete, so it was
  built in one direction and wired into the driver only after end-to-end
  comparison was possible.
- Observed quirks are marked as compatibility requirements in the source, so
  a future reader does not tidy them away.
- Genuine accepts across sessions on the corpus went from 65.5% with the best
  hand built rule to 70.3%, with no accept between different fingers in 9489
  attempts.
- Where the vendor behavior is not known, the gap is stated instead of
  filled: two items remain open in `docs/RESEARCH.md`.

## Evidence

`src/cb2000_engine*`, `docs/RESEARCH.md` ("What the Windows driver does" and
the corpus table), `docs/PROTOCOL.md` ("What the matching engine tells the
driver"). The differential validation is
[0011](0011-measure-behavioral-agreement.md).
