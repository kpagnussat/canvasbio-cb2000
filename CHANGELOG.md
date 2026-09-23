# Changelog

## 1.0.1

- The replies the driver keeps from the sensor (the coverage register after
  the detection and after each image, and the interrupt status) are filed by
  a tag on their entry in the command table, instead of by matching the
  sequence name and the position of the read. Nothing changes on the wire or
  in any decision. Before, renaming a sequence or reordering a table would
  have turned the coverage check off without any error: a missing coverage
  reply counts as covered, so an empty touch would reach the matcher, which
  refuses it as a retry.
- Tested on the sensor with the device test, with the debug output checked
  for the coverage replies. The openSUSE, Fedora and Arch packages were not
  installed again on their live images for this release: they are built from
  the same packaging as 1.0.0, which was, and the change touches nothing that
  differs between distributions.

## 1.0.0

First release. The repository was rebuilt from scratch for it, so there is no
useful line by line diff against anything. What follows is what 1.0.0 is, and
what changes for anyone who used the earlier R2.5 snapshot.

### What it is

- A libfprint **TOD** module for the CanvasBio CB2000, USB `2df0:0003`.
  Your distribution's libfprint stays in place and other fingerprint readers
  keep working.
- Four packages, all built from the same source tarball, each compiled
  against its own distribution's libfprint-tod: a `.deb` for Ubuntu 26.04,
  an `.rpm` each for openSUSE Tumbleweed and Fedora 44, and a `PKGBUILD`
  for Arch. All four have driven the sensor through `fprintd`; the `.deb`
  is the one the accuracy figures were measured with. The README says what
  the other three cost on each distribution.
- Matching is handled by code written independently for this project, using
  documented behavior from the sensor's Windows driver as its compatibility
  target. The two implementations were compared record by record
  (`docs/RESEARCH.md`, "Differential comparison").
  An 80x64 frame yields too few minutiae for libfprint's own matcher.
- Enrollment fuses 15 counted touches into one template, the way the Windows
  engine does, and asks you to move your finger when a touch adds too little
  new area. Verify and identify try each frame of a touch and accept on the
  first match. An unusable touch (off centre, wet, too little ridge area, or
  refused by the sensor) is a retry; only a usable touch that matches nothing
  is a no match.
- The libfprint capture action hands back the frame the sensor read, with no
  quality gate of its own, and asks for another touch only when the capture
  kept no frame at all. Enrollment and matching never had one either: the
  engine judges every touch, as the Windows engine does.
- Plain C with GLib. No OpenCV, no C++ runtime, no third-party matcher.
- `2df0:0007` is deliberately not bound: it is a Realtek match-on-chip sensor
  with a different protocol
  ([#3](https://github.com/kpagnussat/canvasbio-cb2000/issues/3)). It is
  tracked in the
  [linux-fingerprint-drivers](https://github.com/jedbillyb/linux-fingerprint-drivers)
  catalogue, and its support belongs in libfprint's own `realtek` driver.
- Fingerprint images are never written to disk unless a developer opts in
  with `CB2000_DEBUG_IMAGE_DIR`.
- Measured on a private corpus of one person's fingers: the right finger
  accepted in 70.3% of decided attempts across sessions, no accept between
  different fingers in 9489 attempts. Fusing the same enrollment touches in
  a different order does produce one such accept, and the vendor engine
  produces it too (`docs/RESEARCH.md`). One person, one device: see the
  limitations in the README before trusting it with anything.

### Coming from R2.5

- **The package is a different one.** R2.5 shipped `libfprint-2-2` version
  `1:1.94.10+canvasbio.202604201514`, which replaced the system libfprint
  with a build that had the driver compiled inside. 1.0.0 is
  `libfprint-2-tod1-canvasbio-cb2000`, so `apt` does not treat it as an
  upgrade, and the old build does not load TOD modules at all. Restore your
  distribution's libfprint first; the README has the commands.
- **Enrolled fingers must be enrolled again.** A print now holds the engine's
  template instead of captured frames, so an older print ends verify with a
  "data invalid" error. `fprintd-delete`, then enroll.
- No polkit rule and no world-writable udev rule are installed any more.
- The unsupported in-tree build (`packaging/in-tree/`) is pinned to libfprint
  `v1.94.100`.
