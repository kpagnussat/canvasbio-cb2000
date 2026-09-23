# canvasbio-cb2000

A libfprint driver for the **CanvasBio CB2000** fingerprint sensor, USB
`2df0:0003`, the reader built into the power button at the top right of the
keyboard on the Samsung Galaxy Book2 360 and Book3 360.

## TL;DR

The sensor produces 80x64 grayscale images, each covering about 6 x 5 mm of
fingertip. A touch normally needs one image, but the capture plan may read up
to four and gives the matcher at most two. That small window is the central
problem: it is far too little for libfprint's minutiae matcher. This driver
therefore brings its own: a matcher written independently from documented
behavior observed in the sensor's Windows driver, with the same 15-touch
template and the same rules for when to ask you to touch again. It installs
as a libfprint **TOD** module, so your distribution's libfprint stays where
it is and any other fingerprint reader keeps working.

On Ubuntu 26.04:

```sh
sudo apt install ./libfprint-2-tod1-canvasbio-cb2000_1.0.0_amd64.deb
fprintd-enroll
fprintd-verify
```

Fifteen touches to enroll, lifting the finger between them. Then try a finger
you did not enroll and make sure it is refused.

**If your laptop is a Galaxy Book5 360, this is not your driver.** That
machine carries `2df0:0007`, a Realtek match-on-chip sensor that speaks SDCP:
the matching happens inside the chip and the protocol has nothing in common
with this one. The place to go is the catalogue kept by jedbillyb,
[linux-fingerprint-drivers](https://github.com/jedbillyb/linux-fingerprint-drivers),
which tracks that device along with many others;
[issue #17](https://github.com/jedbillyb/linux-fingerprint-drivers/issues/17)
is its entry. Support for a match-on-chip reader belongs in libfprint's own
`realtek` driver, not here.

## Did you install the R2.5 package? Read this first

The package published in the old R2.5 release was not a TOD module. It was
called `libfprint-2-2`, version `1:1.94.10+canvasbio.202604201514`, and it
**replaced your distribution's libfprint** with a build that had the driver
compiled inside. Two things follow, and neither announces itself:

- the 1.0 package has a different name, so `apt` does not see it as an
  upgrade and you end up with both installed;
- that old libfprint build does not load TOD modules at all, so installing
  1.0 on top of it does exactly nothing, with no error message anywhere.

Check what you have and put the distribution's library back before
installing 1.0:

```sh
dpkg -s libfprint-2-2 | grep ^Version    # a +canvasbio version is the R2.5 build
apt policy libfprint-2-2                 # what your distribution ships
sudo apt install --allow-downgrades libfprint-2-2=<that version>
```

Prints enrolled with R2.5 cannot be read by this driver either: the template
format changed. Run `fprintd-delete` for your user and enroll again.

## Supported setup

| Distribution | Package | Before installing | Tested on hardware |
|---|---|---|---|
| Ubuntu 26.04 LTS and flavours on the same base (Kubuntu, Xubuntu, ...) | `.deb`, `libfprint-2-tod1-canvasbio-cb2000` | nothing, `apt` pulls in `libfprint-2-tod1` | yes |
| openSUSE Tumbleweed | `.rpm`, module only | nothing, the `libfprint-2-2` in the OSS repository already carries TOD | yes |
| Fedora 44 (43 untested) | `.rpm`, module only | a libfprint with TOD from a third party repository, plus an SELinux policy. Read the warning below | yes |
| Arch and derivatives | `PKGBUILD` | `libfprint-tod` from the AUR. Read the warning below | yes, on Arch itself |

The four packages are built from the same sources with the same rules. Every
touch behind the figures in this README went through an Ubuntu userspace,
which is what the `.deb` targets. The other three were each installed from the
release, with the commands below, on the live image of their own distribution
(openSUSE Tumbleweed, Fedora 44 with SELinux enforcing, and Arch), in a
virtual machine with the sensor passed through. On each one `fprintd` loaded
the module and the reader enrolled a finger, accepted it 5 times out of 5,
refused a finger that was not enrolled 5 times out of 5, and survived a
cancelled verify. That is a check that the package works end to end on its
distribution, not an accuracy figure; the accuracy figures are the ones below.

**Fedora and Arch replace your system libfprint.** On those distributions the
library with TOD support is not the one your distribution ships: it declares
a conflict with it, so installing it swaps the libfprint of the whole system
for one that comes from somewhere else, including for security updates. That
is a real cost and it is your call to make. To undo it, remove the TOD
library and reinstall your distribution's `libfprint`. On Fedora there is a
second obstacle: with the default SELinux policy `fprintd` will not load TOD
modules at all, so a policy package such as `fprintd_tod_selinux` is needed
before anything works.

Installing the driver into a container, a distrobox, a hand-patched libfprint
tree or the in-tree build below are not supported paths, and reports from
them cannot be investigated usefully because too many parts differ from a
packaged install. The throwaway container of *Try it without installing*
below is a different thing: it installs nothing, and it is how to try the
reader before committing to any of this.

### Installing on the other three

Everything below comes from the Releases page and was built from the same
source tarball as the `.deb`.

**openSUSE Tumbleweed.** The `libfprint-2-2` in the OSS repository is already
built with TOD, so the module is all you need and nothing about your
libfprint changes:

```sh
sudo zypper install --allow-unsigned-rpm ./libfprint-2-tod1-canvasbio-cb2000-1.0.0-1.opensuse.x86_64.rpm
```

The package is not signed. Without `--allow-unsigned-rpm`, `zypper` stops at
"Package header is not signed!" and its default answer is to abort. `zypper`
pulls in `libfprint-2-tod1` and, as a recommended package, `fprintd`.

**Fedora 44.** The rpm is built on Fedora 44 and its dependencies are
resolved there; nobody has tried it on 43. Fedora's libfprint has no TOD
support, so the
library comes from a third party repository, and the default SELinux policy
stops `fprintd` from loading TOD modules at all. Both come from the same
COPR:

```sh
sudo dnf copr enable ferdiu/libfprint-tod
sudo dnf install --allowerasing ./libfprint-2-tod1-canvasbio-cb2000-1.0.0-1.fc44.x86_64.rpm
```

That pulls in `libfprint-tod`, which **takes the place of your system
libfprint**, and `fprintd_tod_selinux`, the policy. `--allowerasing` is what
lets `dnf` remove the system `libfprint` to make that swap; without it `dnf`
refuses the whole transaction with a conflict between the two. The policy is a hard
dependency of this package on purpose: without it the module installs, the
reader stays dead, and nothing anywhere says why. Two other COPRs carry a
TOD libfprint ([grahamwhiteuk](https://copr.fedorainfracloud.org/coprs/grahamwhiteuk/libfprint-tod)
and [quantt](https://copr.fedorainfracloud.org/coprs/quantt/libfprint-tod)),
but neither ships the policy, so with those the dependency does not resolve.

**Arch and derivatives.** The release carries a `PKGBUILD` rather than a
built package, which is how modules for TOD are distributed there. Install
`libfprint-tod` from the AUR first, with your usual helper, and note that it
**takes the place of your system libfprint**. Then, in a directory with the
`PKGBUILD` from the release:

```sh
makepkg -si
```

The `libfprint-tod` recipe checks the signature of the libfprint tag it
builds from. If your helper does not import the key by itself, `gpg
--recv-keys D4C501DA48EB797A081750939449C2F50996635F` does. `fprintd` is only
an optional dependency of this package, so install it too if it is not there
yet (`pacman -S fprintd`).

## Try it without installing

The repository carries an on-device check that builds and runs in a throwaway
container. It talks to libfprint directly, with no fprintd, no D-Bus and no
polkit: it loads the module, enrolls a finger, then asks for that finger and
for one you did not enroll. Nothing is installed on the machine and nothing
is stored, the template lives in memory and dies with the process.

It needs podman or docker, the reader, and root, because the reader's device
node belongs to root. fprintd holds the reader while it runs, and the desktop
session starts fprintd again over D-Bus as soon as it is stopped, so stop it
and start the check in one command, from a clone of this repository:

```sh
git clone https://github.com/kpagnussat/canvasbio-cb2000
cd canvasbio-cb2000
sudo sh -c 'systemctl stop fprintd; exec tests/device/run.sh'
```

`--detect-only` stops once libfprint has found and opened the reader, with
no traffic to the sensor yet, and `--verify N` and
`--impostor N` change how many touches each half asks for, 5 by default.

One line of the output looks worse than it is: `Failed to disable USB
persist` is libfprint writing to `/sys`, which is read only inside the
container. It is expected and changes nothing about the reader.

This is also the report this project most needs from another machine, since
it ends with two counts: how often your enrolled finger was accepted, and how
often another finger was.

## Repository map

| Area | Files |
|---|---|
| libfprint driver | `src/canvasbio_cb2000.c`, `src/cb2000_device.h`, `src/cb2000_tod_entry.c`, `src/cb2000_tod.map` |
| USB protocol | `src/cb2000_protocol.{c,h}`, `src/cb2000_tables.c` |
| Capture decisions | `src/cb2000_core.{c,h}`, `src/cb2000_image.h` |
| Print storage | `src/cb2000_print.{c,h}` |
| Matching engine | `src/cb2000_engine*.{c,h}` |
| Everything else | `docs/` (protocol, research, development notes and decisions), `tests/offline/` (offline tools), `tests/device/` (the on-device check), `packaging/` |

The engine files know nothing about USB or about libfprint: they work on
plain buffers, which is what lets the offline tools measure exactly the code
the driver runs.

<details>
<summary>File by file, all 21 files in <code>src/</code></summary>

| File | What it holds |
|---|---|
| `canvasbio_cb2000.c` | The libfprint driver: device class, open and close, the capture, enroll, verify and identify actions, the state machines that drive them, and the debug image dump |
| `cb2000_device.h` | Driver internal state and the timing and retry constants that go with it |
| `cb2000_tod_entry.c` | The TOD module entry point, the single symbol `fprintd` looks up |
| `cb2000_tod.map` | Linker version script, so that entry point is the only symbol the module exports |
| `cb2000_protocol.h` | USB identifiers, transfer helpers and the shape of a command table, plus why `2df0:0007` is deliberately not bound |
| `cb2000_protocol.c` | The sequence runner: sends each command of a table, reads the replies, waits on the conditions a table declares |
| `cb2000_tables.c` | Data only: the command tables, command by command, as traced from the Windows driver |
| `cb2000_core.h` | The capture plan, as constants and declarations |
| `cb2000_core.c` | How many images a touch takes and with which gain setting, and in which order the frames reach the engine |
| `cb2000_image.h` | Sensor geometry: frame size and the resolution reported to libfprint |
| `cb2000_print.h` | The two calls that move a template in and out of an `FpPrint` |
| `cb2000_print.c` | Print storage: the template buffer inside a versioned GVariant, with the frame size recorded so a print from another sensor is never handed to the engine |
| `cb2000_engine.h` | The engine's contract: records, templates, the stage list, and the vendor numbers, which only mean anything together |
| `cb2000_engine.c` | The extractor: contrast normalization, smoothing, segmentation and orientation field, quality, integral image, keypoints, binarization, edge trim, mask |
| `cb2000_engine_minutiae.c` | Endings and forks of the binary image, found with the pattern table, confirmed on the contour, then pruned by the vendor's chain of rules |
| `cb2000_engine_pet.c` | Keypoint pairing between a template node and a probe: descriptor picks, neighbour trees, the rigid motion that most pairs agree with |
| `cb2000_engine_score.c` | The pixel level check of that motion, the score the minutiae scale, and the decision bar that depends on the pair count |
| `cb2000_engine_compare.c` | Extract and compare as the engine offers them, including the node record every comparison actually goes through |
| `cb2000_engine_bytes.h` | Little-endian accessors of the template format, shared by the two files that read and write it |
| `cb2000_engine_enroll.c` | Enrollment: pairing a touch against the stored nodes, fusing it into the one it matches, and the template buffer byte for byte |
| `cb2000_engine_adapter.c` | What a touch means: which refusals are a retry and which is a real no match, and the counted 15 sample enrollment with its "move your finger" hint |

</details>

For the how and the why:

- `docs/PROTOCOL.md`, the wire: commands, sequences, registers, image framing.
- `docs/RESEARCH.md`, what the investigation established, including how the
  independent implementation was compared with the vendor engine and the
  limits of that comparison.
- `docs/adr/`, one decision per file, with its context and its consequence.
- `docs/the-road-so-far.md`, a short note on what was tried before this
  matcher and why it was dropped.
- `docs/DEVELOPMENT.md`, building it and running the offline tools.

## Considerations

### Enrolling, and how to touch

Enrollment counts **15 touches**. Lift the finger between them and shift it a
little each time, so the template covers more of the fingertip than a single
placement would: the driver refuses a touch that adds too little and asks you
to move your finger. Expect 15 to 18 touches in practice, counting refusals;
it can feel rather longer around touch twelve.

For login and `sudo`, enable the PAM module with `sudo pam-auth-update
--enable fprintd`. The capture area is small enough that some touches share
little skin with the template, so allow yourself a few attempts.

On GNOME the fingerprint appears in Settings once a finger is enrolled. On
KDE Plasma there are two quirks worth knowing: the lock screen accepts the
fingerprint on its own, without `pam-auth-update`, just touch the sensor
while it is locked; the SDDM login screen only uses it after
`pam-auth-update --enable fprintd`, and only once you press Enter with the
password field empty, since it never asks for the finger.

To remove everything: `fprintd-delete <user>`, then `sudo apt remove
libfprint-2-tod1-canvasbio-cb2000`.

### What is stored, and what is not

The print that fprintd keeps for each enrolled finger, under `/var/lib/fprint/`
and readable only by root, holds the matching template: the binarized ridge
pattern of the enrolled area with its keypoints and minutiae. There is no
grayscale image in it, but a ridge pattern is still a picture of your
fingerprint. `fprintd-delete` removes it.

One thing to be aware of: the driver has a debug variable,
`CB2000_DEBUG_IMAGE_DIR`. It is **off by default** and writes nothing unless
you set it. If you do set it, every captured frame is written to that
directory as an image file, which means readable pictures of your fingerprint
sitting in a folder. The directory is created with mode 0700. Use it only to
diagnose something, and delete the files afterwards.

### Known limitations

**Treat this sensor as a convenience unlock, not as strong authentication.**
This is the sober part of the README, but it matters. The warning is not
modesty; it follows from three things. A fourth is worth saying plainly:
neither the vendor's driver nor this one has any liveness or fake finger
detection, because there is none in the hardware or in the vendor's engine to
implement. What reaches the matcher is whatever the sensor read.

The capture area is about 6 x 5 mm. Partial prints that small are the exact
setting studied by Roy, Memon and Ross in
[MasterPrint: Exploring the Vulnerability of Partial Fingerprint-Based
Authentication Systems](https://doi.org/10.1109/TIFS.2017.2691658) (IEEE
Transactions on Information Forensics and Security, 2017), who showed that
with enough enrolled impressions per finger one can find partial prints that
impersonate a large fraction of users. Nothing in this driver changes the
size of the window the hardware gives it.

Different fingers of the same person are not as independent as folklore
holds. Holt measured the correlation between ridge counts on different
fingers as early as 1951
([The correlations between ridge-counts on different fingers](https://doi.org/10.1111/j.1469-1809.1951.tb02481.x),
Annals of Eugenics 16), Li and colleagues traced fingerprint patterns to limb
development genes and recovered the old correlation between the middle three
digits from genetics
([Limb development genes underlie variation in human fingerprint patterns](https://doi.org/10.1016/j.cell.2021.12.008),
Cell, 2022), and Guo, Ray, Izydorczak, Goldfeder, Lipson and Xu trained a
network that tells whether two prints from *different* fingers came from the
same person, well above chance
([Unveiling intra-person fingerprint similarity via deep contrastive
learning](https://doi.org/10.1126/sciadv.adi0329), Science Advances, 2024).
With the earlier matchers of this project the index and middle fingers of the
same hand did accept each other. The current engine refused every such
attempt on the offline corpus and in live use, but that is not a property of
the engine: enrolling the same touches in a different order, one middle
finger touch is accepted by one template in 5 of 20 orders, and the vendor's
own engine accepts it in the same five. A 6 x 5 mm window of two correlated
fingers is a thin thing to rely on.

And the measurements here are from **one person, one laptop, one sensor
unit**. On a private corpus of 8 fingers over two sessions on different days,
the driver accepted the right finger in 70.3% of decided attempts across
sessions and accepted none of 9489 attempts between different fingers. Those
numbers say the engine behaves; they are not an error rate. ISO/IEC 19795-1,
the standard for testing biometric performance, would not take a single
subject corpus as a measurement of error rates at all, and FIDO's biometric
requirements ask for a false accept rate of 1 in 10000 with a false reject
rate under 3% measured over a couple of hundred test subjects. This project
has one subject. By the rule of three, zero false accepts in 9489 comparisons
bounds the rate at roughly 3 in 10000 with 95% confidence, and only for this
person's fingers.

Those figures come from this driver's own captures, so they cannot say
whether it reads the sensor as well as the vendor's driver does. That was
measured apart, by alternating counted touches between the Windows driver and
this one on the same finger, one session at a time: over three sessions, 59
of 60 genuine touches accepted on Windows against 54 of 60 here, and neither
side accepted a finger that was not enrolled, 30 attempts each. That is a
comparison between two drivers and not a second accuracy figure: it is live
use, one sitting at a time, while the corpus replays touches recorded on
different days with the finger shifted on purpose. Both sides ran in a
virtual machine with the sensor passed through.

The six touches the Linux side failed point to one weak enrollment rather
than a systematic capture-side disadvantage. That is not a reading of the
counts: the host recorded the USB traffic of both machines, so every frame
either driver read is recoverable, and replaying all of them through this
engine puts the two capture sides level. Against the template built from
Windows touches, the frames this driver read score 60 of 60 and the vendor
driver's own score 58 of 60; against the template built here, both score 54.
`docs/RESEARCH.md` takes it apart.

If you want the full story of what was measured and how,
`docs/RESEARCH.md` has the tables and the method.

### Unsupported: other distributions, from source

`packaging/in-tree/build.sh` builds libfprint `v1.94.100` with this driver
compiled inside it. `meson install` puts it under `/usr/local`, where on
Debian and Ubuntu it takes precedence over your distribution's libfprint and
stays through updates until you uninstall it, and where on other
distributions it may not be picked up at all. It exists because it is how the
driver was developed, not because it is a good way to install it.

## Provenance

This is a non-commercial, independently written implementation based on USB
traces, device dumps and documented behavioral findings gathered for
interoperability. No vendor source code, binaries, decompiler output, tables
or data files are copied or redistributed. The vendor's Windows package is
used only as a behavioral reference and test oracle; the method and the
derived technical facts are documented in `docs/`.

The driver was written with the help of an AI assistant, mainly for pattern
analysis of the Windows driver's behavior and for keeping this pile of, ahem,
scattered annotations coherent. Every decision was the maintainer's, every
protocol fact came out of a trace, and every number here came out of a harness
run or a dump comparison rather than out of a model. The provider and the
model are not named because they are not relevant to evaluating the driver.
The methods, measurements and known limitations are documented in `docs/`.

## Reporting

Open an issue with the bug template. It asks for a debug log of fprintd,
whose `Opening CanvasBio CB2000 device [...]` line carries the driver
version.

The same form is the right place to report that the driver **works** for you,
especially on a distribution or release not listed above, or on a Galaxy
Book model not listed here. Two facts this project cannot produce on its own are a
second machine and a second person's fingers. Reports from other machines and
people are what will turn the numbers above into something more useful than
one careful case.

## License

LGPL-2.1-or-later. See `COPYRIGHT` for the details and `LICENSE` for the
license text.
