# Decision records

One decision per file, numbered in the order the decision was taken. Only the
decisions that 1.0 actually rests on are recorded here; the ones that were
tried and dropped along the way are in `docs/the-road-so-far.md`, and the
facts they were based on are in `docs/PROTOCOL.md` and `docs/RESEARCH.md`.
The point is not ceremony. It is to keep a future cleanup from rediscovering
an old mistake under a better variable name.

The boundary that keeps these three from becoming copies of each other: the
decision lives in the record, the fact lives in the protocol or research
document, the story lives in the development note. They point at each other
rather than repeating.

| # | Decision |
|---|---|
| [0001](0001-match-on-the-host.md) | Match on the host instead of with libfprint's minutiae matcher |
| [0002](0002-no-images-on-disk.md) | Never write fingerprint images to disk by default |
| [0003](0003-ship-as-a-tod-module.md) | Ship as a libfprint TOD module, not as a libfprint build |
| [0004](0004-measure-with-a-corpus.md) | Measure with a private corpus and an offline harness |
| [0005](0005-do-not-bind-0007.md) | Do not bind 2df0:0007 |
| [0006](0006-observed-behavior-as-compatibility-target.md) | Use observed behavior as the compatibility target |
| [0007](0007-document-implementation-provenance.md) | Document implementation provenance |
| [0008](0008-fifteen-touches-no-update.md) | Fifteen counted touches, and no template update after a match |
| [0009](0009-drop-opencv-and-sift.md) | Drop OpenCV, SIFT and the code derived from SIGFM |
| [0010](0010-print-holds-the-template.md) | A print holds the engine's template, not captured frames |
| [0011](0011-measure-behavioral-agreement.md) | Measure behavioral agreement with differential testing |
| [0012](0012-one-tarball-four-packages.md) | One source tarball, four packages, each built on its own distribution |
