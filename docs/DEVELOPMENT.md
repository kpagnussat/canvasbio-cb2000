# Development

## Sources of truth

Each fact lives in exactly one place. If two places disagree, the one listed
here wins and the other is a bug. This is not just tidiness. When several
files all claim to be right, reverse-engineering notes become folklore
surprisingly quickly.

| Topic | Authority |
|---|---|
| Driver behavior | `src/` |
| Capture plan (images per touch, capture setting, frame order) | `src/cb2000_core.c` |
| Enroll, match and refusal rules (counted touches, move-finger rule, accept rule) | `src/cb2000_engine_adapter.c`, `src/cb2000_engine_score.c` |
| USB protocol facts (commands, sequences, timings, descriptors) | `docs/PROTOCOL.md`, every entry backed by a trace or a device dump, or marked *inferred* |
| Build and build options | `meson.build`, `meson_options.txt` |
| The release version | `meson.build`; `packaging/version.sh` refuses to build when the four packages disagree with it |
| Ubuntu package (the supported one) | `packaging/debian/` |
| Fedora and openSUSE packages | `packaging/rpm/`, one spec with the differences between the two in `%if` |
| Arch package | `packaging/arch/PKGBUILD` |
| The source every package is built from | `packaging/make-tarball.sh` |
| Unsupported in-tree build | `packaging/in-tree/` |
| Release history | `CHANGELOG.md` |
| Research history, measured results, negative results | `docs/RESEARCH.md` (explains past decisions; `src/` wins on current behavior) |
| Why a decision was taken, and what it costs | `docs/adr/`, one file per decision, numbered in the order they were taken |
| How the project got here, as a story | `docs/the-road-so-far.md` (narrative only; it states no fact the documents above do not) |
| User-facing instructions | `README.md` |

## Rules

These are the guardrails that keep a local fix from quietly changing the
device we think we are implementing.

1. All code, comments and documentation are written in English.
2. Do not change USB command tables, sequencing or timings without evidence
   (a Windows trace or a device capture) recorded in `docs/PROTOCOL.md`.
3. The driver is plain C with GLib and libfprint-tod only. Its matcher is
   the independent implementation in `cb2000_engine*`; do not add other
   matching libraries.
4. The driver never writes fingerprint images to disk unless a developer opts
   in explicitly.
5. Only `2df0:0003` is bound. `2df0:0007` is a different device class
   (Realtek match-on-chip) and must not be added here; `docs/PROTOCOL.md`
   "Descriptors" says where its support belongs.
6. Every decision about a frame lives in `src/cb2000_core.c` (the capture
   plan) and in the engine
   (`src/cb2000_engine*.c`: enrollment, match, refusals), which work on plain
   buffers and depend on neither libfprint nor USB. The driver and the
   offline harness call the same functions, so a rule measured on the corpus
   is the rule the driver runs. Do not decide anything in the state machine.

## Source layout

| File | Contents |
|---|---|
| `canvasbio_cb2000.c` | libfprint glue: the capture cycle and polling state machines, the actions, the engine calls and what they tell the sensor side (gain group, lift wait), reporting results |
| `cb2000_device.h` | device state and the cycle's timings and limits |
| `cb2000_protocol.c/h` | USB: the command sequence runner, transfers, image read, coverage register |
| `cb2000_tables.c` | USB command tables, data only, command by command from the traces |
| `cb2000_core.c/h` | decisions: capture plan (images per touch, capture setting, frame order) |
| `cb2000_engine.c/h` | the independently written matcher, using documented Windows behavior as its compatibility target; this file holds the extraction, stage by stage, on the raw frame, and the header holds the whole engine API |
| `cb2000_engine_minutiae.c` | part of the engine: the skeleton minutiae of its binary image |
| `cb2000_engine_pet.c` | part of the engine: keypoint pairing (descriptor candidates, rigid consensus) |
| `cb2000_engine_score.c` | part of the engine: node score (rotated ridge overlap, minutia factors) and the accept decision |
| `cb2000_engine_compare.c` | part of the engine: node records, and the extract and compare calls |
| `cb2000_engine_enroll.c` | part of the engine: enrollment (touch fusion into nodes) and the template buffer |
| `cb2000_engine_adapter.c` | part of the engine: the Windows adapter's touch logic (position and wet checks, retry or no-match, identify walk, enrollment touches) |
| `cb2000_print.c/h` | print storage: libfprint `FpPrint` to and from the engine's template buffer (print version 3; earlier prints held frames and must be enrolled again) |
| `cb2000_tod_entry.c`, `cb2000_tod.map` | TOD module entry point and exported symbols |

`cb2000_core` and `cb2000_engine` build without libfprint
(GLib only).

`-Dtools=true` also builds two tests, both checked against values worked out
by hand (`meson test -C <builddir>`): `cb2000-engine-test` takes every engine
stage on small synthetic frames, and `cb2000-capture-test` takes the capture
decisions of `cb2000_core.c`, that is the brightness metric, how many images
a touch reads and with which setting, and the order the frames reach the
engine in.

## Build

The build environment is the pinned container in `packaging/Containerfile`
(Ubuntu 26.04 with `libfprint-2-tod-dev`).

```sh
packaging/build-deb.sh        # builds the image, then the release .deb into dist/
```

For a quick compile check, build the image once and compile inside it:

```sh
podman build -t localhost/cb2000-build:ubuntu-26.04 -f packaging/Containerfile packaging
podman run --rm -v "$PWD":/src:ro,Z localhost/cb2000-build:ubuntu-26.04 \
    bash -c 'git config --global --add safe.directory "*" &&
             meson setup /tmp/b /src && meson compile -C /tmp/b'
```

`-Dselftest=true` builds `cb2000-selftest`, which talks to libfprint directly
and checks the driver against the reader in front of you: the module loads,
an enrollment completes, the enrolled finger is accepted and another one is
not. It is never packaged, and `tests/device/run.sh` builds it inside a
throwaway container so nothing is installed on the machine under test. How to
run it is in the README, "Try it without installing".

Unsupported variants:

- `packaging/in-tree/build.sh`: libfprint `v1.94.100` with the driver inside.

## Debugging

- `G_MESSAGES_DEBUG=all` on fprintd shows the per-frame diagnostics.
- `CB2000_DEBUG_IMAGE_DIR=/some/dir` makes the driver write every image it
  reads (1 to 4 per touch, before any processing) there as PGM, named
  `<microseconds>_<action>_raw-g<group>i<setting>.pgm`, where group and
  setting index the capture setting table (docs/PROTOCOL.md "Capture
  settings"; setting 0 normal, 1 dry, 2 wet). Images of touches that the
  coverage gate drops are written too.
  Never enable it on a machine you do not own: these are fingerprint images.

## Linux-only behavior

- A cancel sets pin 1 low at once (fprintd cancels when a client stops or
  goes away); Windows waits for its 2 s idle release. Either way the next
  capture initializes the sensor again.
- The Windows driver's light reset after a power on that does not report
  ready (docs/PROTOCOL.md "Finger detection" step 3) is not implemented: it was
  never observed. The cycle fails instead, and the recovery resets the USB
  device and initializes the sensor, at most 3 times in a row.
- libfprint verify and identify both run the engine's identify, the Windows
  sign-in path; the Windows verify call (a position hint is a no-match
  there) has no fprintd user. fprintd checks for a duplicate before an
  enrollment instead of after its last touch; the matcher is the same.
- An enrollment that ends without a print (cancel, error) is the engine's
  discard. The Windows power events that also write the lift-wait flag are
  not implemented.
- The capture action is not an engine path. It hands back the plan's best
  frame, ignores a touch refused at detection, and waits for the lift after
  every finger (docs/PROTOCOL.md "What the matching engine tells the
  driver").

The finger wait and the lift wait are not on this list: like Windows, they
have no time limit, never re-arm by themselves, and only a cancel ends them
(docs/PROTOCOL.md "Finger detection", "Waiting for the lift").

## Matcher corpus

Matcher changes are measured offline against a private corpus instead of by
touching the sensor after every change. Live testing feels direct, but a
slightly different finger placement can make a bad change look good. Only the
tooling is public; the images never leave private, preferably encrypted,
storage.

- The offline harness `cb2000-harness` (`-Dtools=true`) links `cb2000_core`
  and the engine (no libfprint) and runs, touch by touch, what the driver
  runs: each touch is the sample the driver hands to the engine (the capture
  plan replayed),
  enrollment is the engine's, verify is the engine's identify over one
  template (match, retry or no match), and an identify pass puts every touch
  of one session against the templates of another, as fprintd does for a
  user with several prints. It reports genuine and impostor accept rates
  split by session and hand relation, and writes one TSV row per touch x
  template:

  ```sh
  cb2000-harness /private/corpus /private/pairs.tsv A B
  ```

  `--enroll-order random:<seed>` shows how much a result depends on which
  touches built each template (the default, `capture`, is what the enroll
  action sees). The earlier SIFT matcher's figures (docs/RESEARCH.md) come
  from the same harness run on that matcher before it left the tree. The TSVs hold
  derived numbers only but describe private data; keep them next to the
  corpus.
- Layout: `<corpus>/<finger>/<session>/`, one directory per finger (the eight
  non-thumb fingers) per session, with the raw frames and a `manifest.tsv`
  mapping capture attempts to files and outcomes.
- Protocol: at least two sessions on different days, about 30 touches per
  finger per session, lifting and shifting the finger between touches. One
  session builds the templates, the other provides the probes; same-session
  comparisons overstate accuracy.
- Collect on the machine with the sensor (for example the Ubuntu 26.04 test
  VM), with the `.deb` installed. The collector is built natively there:

  ```sh
  sudo apt install build-essential git meson ninja-build pkg-config \
      libfprint-2-tod-dev libglib2.0-dev libgusb-dev
  meson setup build -Dtools=true && meson compile -C build
  sudo systemctl stop fprintd
  sudo build/cb2000-collect /private/corpus left-index A
  ```
- To use a locally built module instead of the installed one, point
  `FP_TOD_DRIVERS_DIR` at the build directory. `sudo` resets the environment,
  so pass it through `env`:
  `sudo env FP_TOD_DRIVERS_DIR="$PWD/build" build/cb2000-collect ...`

## Release

Every package is built from the same tarball, in a container of the target
distribution, against that distribution's own libfprint-tod headers. Nothing
copies the `.so` from one package into another: `tod_driversdir` is fixed
when libfprint is compiled and differs between distributions, and the TOD ABI
carries no stability promise (`tod_soversion` is just `1`). The repeated
builds are deliberate. It is repetitive on purpose.

1. Bump the version in `meson.build`, `packaging/debian/changelog`,
   `packaging/rpm/libfprint-2-tod1-canvasbio-cb2000.spec` and
   `packaging/arch/PKGBUILD`. `packaging/version.sh` prints it and refuses
   when they disagree.
2. Update `CHANGELOG.md`, then commit. The version the driver reports carries
   the commit, and the tarball refuses to build from a dirty tree.
3. Build, in any order:

   ```sh
   packaging/build-deb.sh            # dist/*.deb, the supported package
   packaging/build-rpm.sh opensuse   # dist/*.opensuse.x86_64.rpm
   packaging/build-rpm.sh fedora     # dist/*.fc44.x86_64.rpm
   packaging/build-arch.sh           # dist/PKGBUILD and dist/*.tar.gz
   ```

4. Attach to the release: the three packages, the `PKGBUILD` and the source
   tarball the `PKGBUILD` downloads. Publish the sha256 of each.
5. Check every package on the sensor before calling it tested. Boot the live
   image of its distribution in a throwaway virtual machine with the reader
   passed through, download the package from the release, install it with
   the exact command the README gives, and run enroll, five verifies with
   the enrolled finger, five with a finger that is not enrolled, and one
   cancelled verify through `fprintd`. A live session is the cleanest test
   there is: nothing is installed but what the README says. The README's
   "Tested on hardware" column only says yes after this, and a command that
   needs a flag or an answer the README does not give is a README bug.

The Arch recipe is checked by building it, since no package is produced here:

```sh
podman run --rm -it docker.io/library/archlinux:latest bash
# in the container: build libfprint-tod from the AUR as a normal user, then
# run makepkg on dist/PKGBUILD. Building libfprint-tod checks the signature
# of the libfprint tag, so import the upstream key first.
```

The `.deb` and the two `.rpm` builds take `SOURCE_DATE_EPOCH` from the commit
date, and the tarball is byte for byte reproducible: building the same commit
twice gives the same sha256.
