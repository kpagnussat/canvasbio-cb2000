# The road so far for the CanvasMancer

A short development note: what was tried before the current matcher, why each
attempt was dropped, and where the driver stands now. The facts live
elsewhere, in `docs/PROTOCOL.md` and `docs/RESEARCH.md`, and each decision
that survived into 1.0 has its own file under `docs/adr/`. This is the route
between them, including why the discarded approaches looked reasonable at
the time.

## The problem

The sensor sits in the power button of a Galaxy Book 360, it answers to
`2df0:0003`, and on Linux nothing claimed it. The protocol came out of USB
captures of the Windows driver, command by command, until the sensor woke up
and took an image: 80 by 64 pixels, about 6 by 5 mm of fingertip. That image
size is the whole story of everything that follows.

## NBIS: too little print to work with

libfprint matches image devices with NBIS, the NIST minutiae package, which
needs enough ridge endings and bifurcations spread over enough area to form a
stable constellation. A window this small yields a handful, sometimes three,
and upscaling does not invent more. So the comfortable path, where a driver
just captures and lets the framework decide, was closed from the start.

## SIFT, SIGFM and the OpenCV mosaic: tuned on the sensor

What followed was a matcher built by hand: first a normalized cross
correlation hybrid, then SIFT keypoints, then SIFT over a mosaic that
stitched several touches into a bigger picture of the finger. It was built on
OpenCV, with code adapted from the SIGFM project, and it is what the first
public snapshot shipped. Each version was tuned over many rounds of testing
on the hardware: change a gate, run enrollments and verifies on the sensor,
record what was accepted and what was refused, change it again. Much of the
capture and protocol handling the driver still uses was debugged along the
way.

Two things killed it. The first is that every one of those designs needed
thresholds, and the thresholds interacted, so a bar that held on one set of
templates let impostors through on another. Live touches also cannot be
replayed: between two versions of the same matcher the numbers moved from
25% genuine accepts with 14% false accepts to 81% with none, and there was no
way to tell how much of that came from the code and how much from where the
finger landed. Once there was a recorded corpus and an offline harness to
replay it, the mosaic matcher measured 2.9% of accepts between different
fingers, far too many for a reader that unlocks a laptop. The second reason
is cost: OpenCV meant a C++ runtime and a set of versioned library
dependencies that pinned the package to a single distribution release.

## Where it stands

The current matcher is an independently written implementation of behavior
observed in the matching engine distributed with the vendor's Windows
package. It follows the documented stages, thresholds, rounding rules and
observed quirks because changing one part would make the comparison
meaningless. The vendor tuned that pipeline against a corpus unavailable to
this project, so the observed behavior is preserved instead of being
recalibrated against the much smaller local corpus.

The implementation was written for this project from documented behavioral
findings gathered for interoperability. Only the project's own source and
the derived technical facts are published here; the vendor package is used
as a reference and test oracle and is not redistributed.

Agreement is measured through differential comparison: both implementations
receive the same records and their outputs are compared field by field. The
method, the volumes and the one measured divergence are documented in
`docs/RESEARCH.md`. OpenCV, SIFT and the SIGFM-derived code are gone, and the
driver is plain C with GLib.

On the author's own corpus the matcher accepts the right finger in about
seven of ten decided attempts across sessions. With enrollment touches in
capture order it accepted no wrong finger. When the same touches were fused
in 20 shuffled orders, one touch was accepted by one template in five of
those orders; the vendor engine produced the same result on the same inputs.

The obvious follow-up question, whether this driver gives the engine worse
frames than the vendor's driver does, was tested separately. The same
sensor was passed in turn to Windows and Linux within each session. Across
three sessions, Windows accepted 59 of 60 genuine touches and Linux accepted
54 of 60. The host recorded the USB traffic of both sides, so the frames
themselves could be replayed afterwards through the same engine, and that is
where the gap resolves: against the template built from Windows touches, the
frames captured here score 60 of 60 and the vendor driver's own score 58 of
60. The difference follows the enrollment, not the capture, which
`docs/RESEARCH.md` sets out in full. These results still cover
one person, one laptop and one sensor unit, which is why the README treats
the reader as a convenience unlock and says plainly what remains unmeasured.

Carry on.
