# CanvasBio CB2000 (2df0:0003): research summary

Measured results and the facts behind the driver's design. USB protocol facts
are in `docs/PROTOCOL.md`; current behavior is whatever `src/` does. Every
accuracy figure here comes from one person on one device. Successes, dead ends
and corrections are kept together here; leaving out the awkward parts would
make the document shorter and the evidence worse.

## Where the facts come from

The USB protocol was rebuilt from captures of the Windows driver; every entry
in `docs/PROTOCOL.md` names the trace or the device dump behind it.

The behavioral reference for the matcher is the vendor's Windows package,
CanvasBio WBF 4.1.14.882 (`CanvasBioFingerprintDriver.dll`,
`CanvasBioFingerprintEngine.dll`), analyzed for interoperability.
The engine reports its own algorithm version as `8.8.2.0U`, which identifies
the behavior studied here; the package version identifies the wrapper. The
analysis covered the capture plan, coverage rule, lift wait, session handling,
matching behavior and the messages the engine sends its driver. Only derived
functional facts are recorded; no vendor source, binary or decompiler output
is included.

## Differential comparison

A differential oracle, a name more forbidding than the method. A C
program compiled for Windows loads the vendor engine DLL, reads a binary file
of inputs, calls one entry point per record and writes every reachable output
field to a second file. The same inputs go through the Linux implementation,
which writes the same layout. A third program compares the dumps field by
field and stops at the first divergence. The inputs are real touches from the
private corpus.

The Windows side first ran in a virtual machine, then under Wine in a
throwaway container, which produced identical dumps.

Three levels, in this order:

1. **Stage functions inside the DLL**, one at a time on the same frame, so a
   divergence points at one stage.
2. **The engine's own API**: extract an impression, match two impressions,
   add an enrollment sample, commit a template.
3. **The adapter entry points** the Windows driver calls (update enrollment,
   identify a feature set, the position and wet refusals). No device is
   needed: the program hooks the DLL's `DeviceIoControl` import, so calls to
   its driver are logged instead of reaching hardware, and capture and
   storage are stubs.

| What was compared | Volume | Differences |
|---|---|---|
| Extractor stages, one at a time | 782 frames | 0 |
| Full extraction (descriptors, minutiae) | 773 extractions, 91099 descriptors, 1236 minutiae | 0 |
| Node records written into a template | 772 records | 0, byte for byte |
| Pairing, score, decision and matched flag | 14747 pairs | 0 |
| Enrollment (return code and template buffer) | 782 touches, 15 commits, 11730 compares | 0 |
| Adapter again, on touches collected after the implementation was frozen, in 21 enrollment orders | 4234 enrollment steps, 164 templates, 33410 verify decisions | 0 steps, 0 templates, 149 decisions on one touch |

One divergence: in the pairing transform, 829 of 14528 floating point values
differ in the last bit. `atan2`, `cos` and `sin` in the runtime the DLL links
against do not round identically to glibc, and the vendor's own sine and
cosine shift with fused multiply add depending on the CPU. Every integer
output, record and verdict in these runs is identical.

The last row is the same level 3 comparison run again, after the implementation
was finished, on inputs it had never seen: every touch of the side by side
battery below, enrolled in capture order and in 20 shuffled orders, each
template then probed by every touch. Enrollment agrees step by step, the
committed templates are byte for byte the same once the random identifier the
vendor writes into each one is masked, and every verify decision agrees except
on one touch, the one whose capture produced no usable image at all. There
this implementation answers "try again" and the oracle answers "no match".
The difference is in the test path: it calls the identify entry point directly
and skips the sample intake used by the Windows driver, where an empty sample
is refused with exactly the "try again" this driver returns. The earlier runs
never fed identify an empty sample, which is how the gap stayed invisible
until these inputs.

What the oracle does not cover:

- **Accuracy.** It shows measured behavioral agreement on these inputs, not
  that the vendor engine is good. That is the corpus table below.
- **Capture.** The USB side has no oracle of this kind, and the corpus was
  collected with this driver's capture, so a worse capture would be invisible
  in the corpus table. It was rebuilt from traces, checked against hardware,
  and measured against the Windows driver twice over: touch by touch in live
  use, and frame by frame through this engine, both under "Side by side with
  the Windows driver".
  The corpus was collected with the capture action, which does not run the
  engine, so its gain group follows the brightness rule alone; in live use a
  failed touch sends the next capture back to the first group. 48 of the
  corpus images were taken in the second group.
- **The input space.** One person, one sensor unit, 782 frames plus the
  touches of the side by side battery. A frame shape neither set produced
  could still diverge, and the empty sample above is one that the first
  runs never reached.

The oracle programs, dumps, extracted tables and corpus stay private. The
vendor binary is not ours to redistribute, and the corpus is biometric data.
The method and measured results above are public so the test can be recreated
with independently obtained inputs and a lawful copy of the vendor package.

## What the Windows driver does

- **Split.** Windows' own sensor and storage adapters, plus two vendor
  binaries: a user-mode driver that captures, and an engine adapter that
  extracts, enrolls and matches. The device only captures and never returns
  a verdict.
- **Capture.** One image per touch normally; a brightness metric asks for a
  second image with a dry or wet gain setting, or switches to a second
  group of settings. The engine gets 1 or 2 raw frames and tries each. No
  background frame, averaging or frame differencing.
- **Coverage.** Register `0x3e` is a 12-zone bitmap. A detection covering
  fewer than 2 zones is not a touch; the engine gets an empty sample.
- **Engine.** Contrast normalization, 8x8 segmentation, orientation field,
  oriented-line binarization, blob keypoints with a rank-order descriptor,
  skeleton minutiae. Keypoint pairs must agree with one rotation plus
  translation; the decision weighs the pair count against the ridge
  disagreement of the aligned binary images.
- **Enrollment.** 15 counted touches (10 + 5 on this sensor) fused into one
  template; from the sixth to the ninth, a touch adding too little new area
  may be refused with "move your finger".
- **After a match.** The engine still merges the accepted touch into the
  template it holds in memory, and can prune a node while doing it. None of
  it survives on this sensor: verify never writes back, identify only stores
  the result when a learn flag is set and that flag is 0 here, and every
  operation reloads the stored template first. The enrolled print never
  changes. This implementation skips the merge, which costs the vendor CPU
  and changes no answer.
- **Refusals.** Off-center, wet and unextractable touches are "try again",
  checked only after every frame failed to match; only a usable image that
  matches nothing is a no-match.
- **Engine to driver.** After a failed touch the next capture starts in the
  first gain group; whether the next capture waits for the finger to lift is
  set per operation (no wait only after activation and after an enrollment
  ends).

## Why not NBIS

libfprint matches image devices with NBIS, which needs enough minutiae spread
over enough area. An 80x64 frame is 6.0 x 4.8 mm of fingertip and yields a
handful, three in some touches, and upscaling adds none.

## Offline corpus

`cb2000-harness` replays a private corpus: 8 fingers, two sessions on
different days, 745 touches, 15 templates (one finger enrolled in one session
only). Each touch probes its own finger's template from the other session
(genuine) and every other finger's template (impostor).

| Matcher | Genuine cross-session, per decided attempt | Per touch | Accepted between different fingers |
|---|---|---|---|
| Mosaic, then gallery vote | 378/592 (63.9%) | 54.7% | 239/8312 (2.9%) |
| Rigid support + ridge judge | 388/592 (65.5%) | 56.2% | 0/8312 |
| Windows engine (current) | 476/677 (70.3%) | 68.9% | 0/9489 |

- The engine gets every touch (no quality gate); its retries (position, wet,
  extractor) are not decisions and PAM does not count them.
- Worst finger 52.1%, best 92.2%. Every template completed within 15 to 18
  touches.
- Identify, every touch of one session against the other session's 7 or 8
  templates: 476 right finger, 0 wrong finger, 252 no match, 17 retries.
- One person, one device, one sensor unit, no other person's fingers.

The first two rows are matchers that are no longer in the tree; they left
with OpenCV, and the figures come from running them on this same corpus.
Why each was tried and dropped is in `docs/the-road-so-far.md`.

## Side by side with the Windows driver

The oracle covers the engine and not the capture, so it cannot say whether
the frames this driver reads are as good as the ones the vendor's driver
reads. That is measured directly, with the same sensor handed in turn to a
Windows virtual machine running the vendor package and to a Linux virtual
machine running this driver, in the same session, on the same skin.

The design was fixed before the first touch. Each session takes 20 touches of
the enrolled finger and 10 of a finger that was never enrolled, on each side,
in blocks that alternate within the session, and the side that starts changes
from session to session. A touch the operator felt was badly placed is
written down and still counted, and each session is reported with and without
those. The decision rule was written down as well: under 10 points between
the sides is no capture problem worth chasing, 15 points or more below is a
capture problem to fix before anything else.

| Side | Genuine | Not enrolled, accepted |
|---|---|---|
| Windows, vendor driver | 59/60 | 0/30 |
| Linux, this driver | 54/60 | 0/30 |

These counts answer which driver reads the sensor better, not how accurate
either one is, and they do not belong beside the corpus table: each session
is live use, one sitting, one skin, while the corpus replays recorded
touches from different days with the finger shifted on purpose.

The three planned sessions are done, n = 60 per side. Session by session,
genuine: 19/20 and 19/20, then 20/20 and 19/20, then 20/20 and 16/20. The
battery ends 8.3 points apart against a rule, written down before the first
touch, that starts caring at 10 and calls for work at 15. No finger that was
not enrolled was accepted on either side, 30 attempts each. Both sides run
in a virtual machine with the sensor passed through, which is not the same
as bare metal. A pilot session with the same design came before them (20/20
and 19/20, 0/10 on each side); it is kept out of the table because the
logging tools changed after it.

### Both sides' frames through the same engine

The counts above are live use, which mixes the capture with the enrollment,
the session and the skin of the moment. The frames themselves were compared
on their own as well. The host recorded the USB traffic of both virtual
machines, so every image either driver read during the battery is
recoverable, and all of them were replayed through this engine against both
enrolled templates: the one built from Windows touches, and the one built
from this driver's touches.

| Genuine probes, right index, n = 60 per row | Windows template | Linux template |
|---|---|---|
| captured by the vendor's Windows driver | 58/60 | 54/60 |
| captured by this driver | 60/60 | 54/60 |

No finger that was not enrolled was accepted in any of the four cells.

Read down the columns, the two capture sides are level: against the Linux
template they tie, and against the Windows template the frames this driver
read score two touches higher than the vendor driver's own. Read across the
rows, the template is what moves the result: the same frames from this driver
go from 54 to 60 when the template changes. The image metrics agree. The
vendor's own brightness metric, session by session, has medians of 135, 143
and 141 on the Windows frames against 129, 144 and 143 on ours, and full 12
of 12 zone coverage appears in 18 of 21, 19 of 20 and 19 of 20 Windows
touches against 19 of 20, 20 of 20 and 19 of 20 of ours. Both sides read one
image in the normal group 0 setting on nearly every touch.

So the live gap is not the capture. It follows the enrollment, which is what
the next section takes apart.

### The evidence points to one weak enrollment

The gap is not spread evenly: the sides tie in the first session, part by
one touch in the second and by four in the third. The third session was
taken apart. Every
frame of both sides went through this project's engine offline. Brightness,
coverage and the capture plan match; over the whole battery, against the
template enrolled on Windows the Linux frames score 60/60 and the Windows
frames 58/60, and against the template enrolled on Linux both sides score
54/60. The frames captured on Linux are the better ones by these counts. The
live failures follow the template that the Linux side verifies against.

That template was built once, in the first session, and every later session
reused it, so an ordinary weak enrollment would look exactly like a capture
problem that only shows up on one side. To separate the two, the six blocks
of the battery were each enrolled as a template of their own with the
driver's own engine, in capture order and in 20 shuffled orders, and every
touch of the battery was then probed against every template. Per template,
over the 20 orders, the accept rate sits between 94% and 99% for the six new
templates and between 87.4% and 92.4% for the suspect one, which is the
worst of the set in every single order, its best order below the median of
all the others. The best template of all, 99%, is built from frames captured
on Linux. Grouping instead by the side that captured the enrollment and the
side that captured the probe, with the suspect template left out, the four
cells land within three points of each other (95.9, 95.5, 97.0, 94.1) and
the rows share touches, so they are not independent. Within the limits of
this battery, the measurements point to one weak enrollment rather than a
systematic disadvantage in the Linux capture path.

A lab variant that ends enrollment at 10 counted touches instead of 15,
which is not the shipped configuration and builds a smaller template, stops
the suspect enrollment being the worst of the set: it comes out above the
other session 1 template and just under the six battery ones. Its weakness
is in what its last five touches added.

### Impostor accepts under shuffled enrollments

In those same 20 shuffled orders, one touch of the finger that was never
enrolled was accepted, by one template, in 5 of the 20 orders: 5 accepts in
8610 decided impostor attempts, all of them the same touch against the same
template, and none in capture order. It is not an implementation divergence.
The vendor engine, fed the same touches in the same orders through the oracle
above, accepts that touch against that template in exactly the same five
orders.

What this says is that the engine's margin against a nearby finger, the
middle finger of the same hand, is thinner than a clean 0 in a table
suggests, and that the order in which touches are fused into a template
moves it. In the real enrollments, and in the live battery, that finger was
accepted 0 times in 30 attempts per side. The lab variant that enrolls at 10
touches, with the roles swapped so the middle finger is the enrolled one,
accepted 0 of 788 impostor attempts in either direction. The reading to take
away is the one in the README's limitations: two fingers of the same hand
through a 6 x 5 mm window are close, and this sensor is a convenience
unlock.

## Corrections to earlier readings

Recorded because each of these was believed, written down, and acted on. The
list is part of the result, not an appendix to hide the untidy bits.

- **Coverage register read as verdicts.** `ff:0b` was taken as a device
  no-match and the other values as retry codes; they are zone bitmaps (11 of
  12 zones). The device never judges a touch.
- **Commands misnamed.** `0xDB 1/1` and `1/0` are GPIO 1 high and low, not a
  factory reset and a deactivate; `0xCA 3/5123` is the SPI length of the
  image read, not the capture trigger (`a9 04 00 00`).
- **Image framing.** 320-byte chunks are a capture tool's 64-byte header
  plus 256 bytes; the image is 80x64, not 192x192, with no stride offset.
- **Background frame.** Subtracting an enrollment background inverted frames
  and made prints differ from probes; Windows has no background.
- **Lift wait.** The old removal wait read the finger line with the detection
  off and always saw the finger lift.
- **Stuck detection.** A USB reset per action and a 12 s re-arm hid a command
  table that broke the bridge's rule; the sensor stays initialized on
  Windows.
- **Enrollment count.** 8, then 14, then the configuration itself: 10 + 5.
- **Adaptive template.** First read as absent, then as "present but switched
  off". Both are wrong: the merge runs on every accepted touch and is thrown
  away, because nothing writes it back on this sensor. The effect on the
  stored print is the same, the mechanism is not, and only the mechanism says
  what an independent implementation may leave out.
- **Tuning piece by piece.** Changing vendor numbers before the full chain was
  implemented measured nothing: the numbers only mean something together.
- **Retries as a fix.** Retrying a no-match gives an impostor attempts PAM
  does not count; only an unusable touch is a retry.
- **Same-hand cross-acceptance** (index and middle finger accepting each
  other) was seen with the earlier matchers, and never with the engine on
  the corpus or in live use. "Never" was then read as a property of the
  engine. It is not: varying the order in which a template's touches are
  fused, one middle finger touch is accepted in 5 of 20 orders, and the
  vendor engine does the same on the same inputs ("Impostor accepts under
  shuffled enrollments").

## Open questions

These are the points the available evidence cannot answer yet, stated here so
that uncertainty does not quietly harden into fact.

1. **Accuracy.** No multi-person data, no second device, no ROC or equal
   error rate.
2. **Resolution.** Which of the two figures in the device table of
   `docs/PROTOCOL.md` is the physical one is not settled. The engine does not
   use either.
3. **Power events.** Windows also sets the lift-wait flag on power events
   (sleep, display); libfprint's suspend and resume are not wired to it.
4. **Upstreaming.** Whether libfprint would take an image driver that carries
   its own engine.
