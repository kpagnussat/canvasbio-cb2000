# CB2000 USB protocol

Facts about the device and its USB protocol. Sources:

- USB traces of the Windows driver captured on a Galaxy Book3 360;
- a device descriptor dump;
- static analysis of the Windows driver package for this device
  (CanvasBio WBF driver 4.1.14.882, INF `USB\VID_2DF0&PID_0003`), done for
  interoperability. Only derived facts are recorded here (request codes,
  register addresses and values, sequences, byte meanings, timings); no code
  from that package is reproduced.

Entries that no trace or code confirms are marked *inferred*. What the Linux
driver does with these facts is defined in `src/`, not here. This document is
the evidence ledger: observed facts first, interpretation labelled, code
elsewhere.

## Device

| Property | Value |
|---|---|
| USB ID | `2df0:0003` |
| Sensor hardware id | `0x23` (register `0xb9`) |
| Image | 80x64 grayscale, 8 bits per pixel, 5120 bytes |
| Resolution | the Windows driver declares 508 ppi in its image record; earlier estimates were about 340 DPI (active area about 6.0 x 4.8 mm). Which one is physical is not settled. |
| Matching | on the host; the sensor only captures and never returns a match verdict |

## Architecture

The USB device is a Cypress USB-to-serial bridge acting as an SPI master. The
fingerprint sensor sits behind it on SPI. All vendor requests on USB are
bridge commands; sensor commands travel as SPI bytes through the bulk
endpoints.

| Transfer | `bmRequestType` or endpoint |
|---|---|
| Control out | `bmRequestType 0x40` (vendor, host to device) |
| Control in | `bmRequestType 0xC0` (vendor, device to host) |
| Bulk out | endpoint `0x01` (SPI bytes to the sensor) |
| Bulk in | endpoint `0x82` (SPI bytes from the sensor) |

| `bRequest` | Bridge function | Parameters |
|---|---|---|
| `0xCA` | SPI transfer | `wValue` 2 = write only, 3 = write and read (full duplex); `wIndex` = transaction length in bytes. The bytes then go out on bulk out, and for `wValue` 3 the same number comes back on bulk in. |
| `0xCC` | SPI status | 4 bytes in. Windows reads it after every register write and ignores the content. |
| `0xDB` | GPIO set | `wValue` = pin, `wIndex` = level |
| `0xDA` | GPIO get | `wValue` = pin; 2 bytes in, the level is byte 1 |

The Windows driver also uses bridge configuration requests (`0xC3` SPI
configuration at 2 MHz with 8-bit words, `0xE2`, `0xB5`/`0xB6` for the
bridge's configuration block, `0xF1`-`0xF5` for its firmware) when the device
starts. They are absent from the traces, which began with the device already
running.

### GPIO pins

| Pin | Role |
|---|---|
| 1 | "Power-button block" line. Windows drives it to 1 while a capture session is active and to 0 about 2 s after the session stops. It resets nothing. |
| 6 | Sensor reset: pulse 1, 0, 1 with 1 ms steps, followed by a full initialization. |
| 7 | Sensor interrupt line. Driven to 0 before arming finger detection, then read with `0xDA` to see whether a finger interrupt fired. |
| `0xfe`, `0xff` (read) | Bridge flags for an image the bridge captured on its own while the host slept (touch-to-wake). Windows reads them at the start of identify captures only (WaitForCaptureImage checks the capture purpose); enrollment and other captures skip them. |

## Sensor commands over SPI

| Bytes | Transfer | Meaning |
|---|---|---|
| `a9 RR VV` | `0xCA` 2/3, then `0xCC` | write the 8-bit register `RR` |
| `a9 RR LO HI` | `0xCA` 2/4, then `0xCC` | write the 16-bit register `RR`, low byte first |
| `4f 80` | `0xCA` 2/2 | raw wake command |
| `a8 RR 00 ...` | `0xCA` 3/n+2 | read `n` bytes of register `RR`. SPI is full duplex, so the first two bytes that come back are filler clocked while the command goes out; the data starts at byte 2. |
| `a8 06 00` + zeros | `0xCA` 3/5123 | read the image: 5123 bytes, pixels in bytes 3..5122 |

So the 4-byte replies of the form `ff 00 SS RR` seen in the traces are
register reads: two filler bytes, then the register value.

Every SPI transaction has its own `0xCA` setup: a write of `n` bytes follows
`0xCA` 2/`n` and is followed by `0xCC`; a read follows `0xCA` 3/`n` and its
`n` bytes are always collected. The five Windows traces keep this rule with no
exception (over 6,600 transfers). The bridge is quite literal about this. An
earlier Linux table broke the rule twice: it sent `a9 09 00 00` right after
the `a8 08` read, still under `0xCA` 3/3, and
it sent `a8 20 00 00` without reading the reply. In both cases the bridge
held the next control transfer for about 765 ms and 257 ms (every capture of
a 2026-09-11 test session); with the setup in place it answers in about
2 ms, as in the Windows traces.

### Registers

| Register | Access | Meaning |
|---|---|---|
| `0x04` | write 16-bit `0000` | start an image capture (this is the capture trigger) |
| `0x04` | write 8-bit `00` | arm finger detection |
| `0x03` | write 16-bit `0000` | stop finger detection |
| `0x03` | write 8-bit `00` | idle |
| `0x09` | write 16-bit `0000` | clear the interrupt |
| `0x0c` | write 8-bit `00` | power on the analog front end |
| `0x0d` | write 8-bit `00` | power off after a detection or a capture |
| `0x01` | write 8-bit `00` | soft reset |
| `0x3e` | write 8-bit `ff` | clear the coverage register |
| `0x5d`, `0x51` | write 16-bit | capture gain/offset setting (see "Capture settings") |
| `0x08` | read 1 byte | interrupt status; bit 3 = finger interrupt |
| `0x20` | read 2 bytes | state: `01 02` powered and ready; `01 00` or `01 07` after detection stopped |
| `0x3e` | read 2 bytes | finger coverage bitmap (see below) |
| `0xb9` | read 1 byte | hardware id (`0x23` for this sensor) |
| `0x06` | read 5120 bytes | image |

### Coverage register `0x3e`

The two data bytes form a 16-bit little-endian value. Its low 12 bits are a
coverage bitmap: one bit per zone of the sensing area, set when the finger
covers that zone. The Windows driver only counts the set bits:

- fewer than 2 zones when a finger interrupt fired: not a touch, try again;
- more than 1 zone after a capture: the image is usable.

No other meaning exists: the register never carries a match result, a retry
code or a placement hint (Windows computes placement hints from the image).
Examples, as the traces show them (`ff 00` filler, then the two data bytes):

| Reply | Value | Zones covered |
|---|---|---|
| `ff 00 ff 0f` | `0x0fff` | 12 |
| `ff 00 ff 0b` | `0x0bff` | 11 |
| `ff 00 f7 0f` | `0x0ff7` | 11 |
| `ff 00 f3 0f` | `0x0ff3` | 10 |
| `ff 00 f0 0f` | `0x0ff0` | 8 |
| `ff 00 30 0f` | `0x0f30` | 6 |
| `ff 00 20 0f` | `0x0f20` | 5 |
| `ff 00 0f 00` | `0x000f` | 4 |
| `ff 00 88 08` | `0x0888` | 3 |
| `ff 00 11 00` | `0x0011` | 2 |
| `ff 00 08 00` | `0x0008` | 1 |
| `ff 00 00 00` | `0x0000` | 0 |

*Inferred:* the zones form a 3x4 grid (bit n = row n/4, column n%4), which
fits the placement hints Windows showed for partial touches such as `0x0011`
and `0x0888`.

## Sequences

### Initialization

1. Optional hardware reset: GPIO 6 pulse.
2. Raw `4f 80`, then `a9 4f 80`.
3. Read `0xb9` (hardware id).
4. Fourteen 16-bit register writes (`0x50 0012`, `0x5f 0000`, `0x4e 0002`,
   `0x60 0021`, `0x61 0070`, `0x62 2100`, `0x63 2100`, `0x64 0804`,
   `0x65 0885`, `0x66 000d`, `0x67 0010`, `0x68 0c00`, `0x6b 7011`,
   `0x6c 0e00`).
5. Clear the interrupt, wait 10 ms.

### Finger detection

1. Detect setting: `0x5d = 003d`, `0x51 = 01a8`.
2. Detection mode: `a9 03 00`, then 16-bit writes `0x38 0001`, `0x10 0060`,
   `0x3b 0014`, `0x3d 0fff`, `0x26 0030`, `0x2f fff6`, and clear the
   interrupt.
3. Power on: `a9 0c 00`, wait 5 ms, read `0x20` until `01 02` (2 reads,
   10 ms apart; the Windows driver then tries a light reset, not implemented).
4. GPIO 7 to 0, wait 1 ms (about 14 ms in the traces), then arm with
   `a9 04 00`.
5. Poll GPIO 7 until it reads 1. Windows sleeps 5 ms between polls (about
   16 ms with the default Windows timer) and has no timeout: only a cancel
   ends the wait.
6. Read `0x08` and clear the interrupt. If bit 3 is clear, arm again
   (`a9 04 00`) and go back to 5.
7. Read `0x3e`. Fewer than 2 zones: `a9 0d 00` and treat it as no finger:
   no image is read. For this sensor the Windows driver then completes the
   capture with one zeroed frame and a frame count the engine reads as 0
   (WaitForCaptureImage), so the engine gets an empty sample.
8. `a9 03 00 00` (stop detection), then read `0x20` up to 20 times, 10 ms
   apart, until `01 00` or `01 07`; then `a9 0d 00`. Not stopped after 20
   reads: arm again and go back to 5.

Steps 6 to 8 follow the interrupt at once: the traces read `0x3e` about 2 ms
after GPIO 7 goes high, and the image read starts about 40 ms after it.
Right after the initialization, an attempt starts with pin 1 high (`0xDB`
value 1, index 1), then, for an identify capture only, the reads of GPIOs
`0xfe` and `0xff`, then steps 1 to 4. The 2026-09-18 trace shows both: the
Windows Hello enrollment reads them for its first capture (the duplicate
check, an identify) and for none of its 20 enrollment captures, while every
identify capture of the battery blocks reads them.


### Waiting for the lift

Before each detection Windows first waits for the finger of the previous
touch to lift (WaitFingerUpCB), with no timeout: only a cancel ends the wait.
It skips the wait when the "skip finger-up wait" flag is set, which the
capture reads first (runContinuousCaptureAREA); the engine sets and clears
that flag, see "Lift wait before the next capture".
The Windows unlock trace with failed attempts shows the wait command by
command after a rejected touch and after every capture:

1. Pin 1 high (`0xDB` value 1, index 1), read GPIOs `0xfe` and `0xff` (identify
   captures only, see "Finger detection"), then detection steps 1 to 3,
   reading `0x20` (`01 02`) about 16 ms after the power on; GPIO 7 to 0.
2. Wait about 16 ms, `a9 04 00` (arm), wait about 16 ms, read GPIO 7.
3. GPIO 7 low: `a9 0d 00`, the finger is up.
4. GPIO 7 high: read `0x08` and clear the interrupt. Bit 3 clear: `a9 0d 00`,
   the finger is up. Bit 3 set (the trace reads `0a`): GPIO 7 to 0,
   `a9 03 00 00`, `a9 0d 00`, wait about 21 ms, `a9 0c 00`, wait about 16 ms,
   read `0x20`, then back to step 2.
5. After the lift, the detection: steps 1 to 3 again (without the pin 1 and
   strap reads), GPIO 7 to 0, wait about 16 ms, `a9 04 00`, then GPIO 7 is
   polled.

The waits are Sleep(5) and Sleep(10) in the driver; the trace shows how
long they last with the default Windows timer. Arming the detection on every
round matters: a finger that stays down raises the interrupt again each
time, while GPIO 7 read without arming (after `a9 0d`) stays low with the
finger on the sensor.

### Capture

1. Capture mode: 16-bit writes `0x10 0100`, `0x26 0000`, clear the
   interrupt; power on as in detection step 3.
2. Capture setting: `0x5d`, `0x51` (see below).
3. `a9 04 00 00` (trigger), clear the interrupt, image read, clear the
   interrupt.
4. Read `0x3e` twice. Windows uses the first read for "covered" (more than 1
   zone) and the second to rank frames. It never compares the two.
5. After the last image: `a9 0d 00`.

### Session

The Windows driver initializes the sensor once and keeps it initialized
across captures: the next attempt starts with the lift wait (or, right after
the initialization, with the detection). When no capture starts within 2 s
of the last one (CommanderThread), it sets pin 1 low (`0xDB` value 1,
index 0) and marks the sensor for a new initialization, which the next
attempt runs with no USB reset; the traces show it as `DB1/0` 2 s after the
last capture. The initialization does not touch the lift-wait flag: its
only writer is called from the engine's IOCTLs, device
preparation and power state changes (D0 entry and exit, suspend) and the
driver's power-event window. So an attempt after an idle release waits for
the lift when the flag says so. The USB device itself is reset only at
device start and to recover from errors. Releasing the device (the traced
disable): pin 1 low, about 100 ms, clear the interrupt, `a9 03 00` twice,
pin 1 low.

### Image read framing

One image is 5123 bytes read as 20 rounds of bulk out then bulk in. The
first bulk out is 259 bytes (`a8 06 00` and 256 zeros), the others 256 zeros;
the bulk-in sizes are 256 x 19, then 259. The pixels are bytes 3..5122 of the
concatenated input, with no inversion or rotation. The first three input
bytes (for example `ff 00 06`) are SPI filler, not pixel data, whatever they
read. The Linux driver requests the same sizes, always drops the first three
bytes, and treats a read of any other size as a protocol error.

Some traces show 320-byte chunks with a 64-byte header; *inferred* to be the
capture tool's per-record header, not device framing.

## Capture settings

Registers `0x5d` and `0x51` select the capture gain/offset. The Windows driver
has a detect setting and two groups of three capture settings:

| Setting | Group 0 (`0x5d` / `0x51`) | Group 1 (`0x5d` / `0x51`) |
|---|---|---|
| Normal | `004d` / `0188` | `003d` / `0168` |
| Dry finger | `004d` / `0187` | `003d` / `0167` |
| Wet finger | `004d` / `018a` | `003d` / `0169` |
| Detect (not a capture setting) | `003d` / `01a8` | |

After each image Windows computes a brightness metric: the 80 columns are
split into 10 bands of 8; in each band it takes the pixels with value 5 or
more, and the band is valid when those are at least 40 % of it; its value is
the mean of 255 minus the pixel value. The metric is the mean over the valid
bands (dropping the lowest and highest when there are 3 or more), or 255 when
no band is valid.

- 80..179: normal. Keep the frame; the attempt ends with 1 frame.
- Above 179: dry. Below 80: wet. Keep the frame and capture a second one with
  the dry or wet setting of the same group; the attempt ends with 2 frames.
- Group switch: when the first image of an attempt in group 0 is dry (or has
  no valid band), Windows switches to group 1 and captures again with its
  normal setting. When the first image in group 1 is wet, it switches back to
  group 0. At most one switch per attempt. The group persists across attempts.
- In group 1, a first image with fewer than 2 covered zones is rejected at
  once.
- With 2 frames, the one whose metric is closer to 120 goes first, unless it
  covers fewer zones.

The matcher receives 1 or 2 frames per attempt and tries each. There is no
averaging, no background frame and no frame differencing.

These settings explain the traces: the group 1 normal setting (`003d`/`0168`)
appears after images judged too bright, often partial touches, and stays in
use for later captures until a wet image switches back. The registration and
the unlock-with-errors traces each show one touch with two images: `004d`/`0188`,
image, two `0x3e` reads, then `003d`/`0168`, trigger, image, two `0x3e` reads,
and only then `a9 0d 00`. There is no power cycle between the images of a
touch.

Further details of the same rule:

- With the first image of an attempt not covered (1 zone or less) or without
  a valid band, and no group switch, Windows captures again with the dry
  setting. If that image is not covered either, it reads one more image with
  the normal setting, drops it and ends the attempt.
- A group-switch image covering 6 zones or more (second `0x3e` read) is kept
  as a fallback frame, used when the attempt ends with no frame.
- In group 1, a first image whose second `0x3e` read shows fewer than 2 zones
  is also rejected at once.
- Frame order: when the first frame's metric is 241 or more and the second's
  239 or less, the second goes first.

The Linux driver follows this rule (`cb2000_core_capture_step` and
`cb2000_core_capture_order`) and starts every open in group 0.

## What the matching engine tells the driver

The Windows engine adapter (`CanvasBioFingerprintEngine.dll`) sends private
IOCTLs to the driver after its decisions. Two of them change what the next
capture does on the wire. Both were identified through static analysis of the
engine and driver (WBF 4.1.14.882), then cross-checked independently. The
relevant call sites and resulting behavior are summarized here.

### Gain group reset (U1)

Engine info 0 or 1 makes the driver set the capture group to 0
(`setAdcTableGroup(0)`), so the next capture starts with the group 0 normal
setting (`004d` / `0188`). The engine sends it:

- after every identify call that got a non-empty sample and did not match,
  whatever the reason: no match on a usable image, finger position, wet
  finger, extractor refusal, enrollment in progress, storage failure, empty
  gallery (one common exit in the engine adapter);
- after an enrollment touch refused with a negative result: empty sample,
  finger position, or every frame of the touch failed.

It is not sent after a match (engine info 4 leaves the group alone), after
an empty identify or verify sample (refused before any decision), after a
"move your finger" refusal, after a counted enrollment sample, or after a
frame the enrollment accepted with code 0. Commit,
discard, activate, deactivate and detach send nothing. The Windows verify
call sends no engine info at all.

### Lift wait before the next capture (U2)

The adapter keeps one flag that the driver reads before each capture: skip
the lift wait, or wait for the finger to lift first. No capture clears it;
only these calls set it.

- **Skip:** activate (device open), enrollment completion (the 15th counted
  sample), commit, and discard, even when no enrollment is open.
- **Wait:** every identify or verify call with a non-empty sample, whatever
  the outcome, a match included (before the enrollment and storage checks);
  every enrollment touch, the empty one and every refusal included (before
  the frame test).
- **Unchanged:** an empty identify or verify sample, the duplicate check,
  clear context, deactivate, detach.

After a match the flag stays on "wait", so a finger left on the sensor is not
read again for the next operation.

### Mapping to libfprint

The Linux driver maps both libfprint verify and identify to the Windows
identify call (the unlock path), so U1 and U2 apply to them as listed above.
fprintd runs an identify over all prints before an enrollment; that identify
sets "wait", so the first enrollment touch waits for the lift, as on Windows.

A touch refused at detection (fewer than 2 zones) and a capture that kept
no frame reach the engine as an empty sample: a retry in verify and
identify, a refused touch in enrollment.

Linux driver status: the driver applies U1 and U2 exactly as listed above
(`cb2000_enroll_submit`, `cb2000_match_submit`) and starts every open in
group 0. The engine's activate (wait skipped) maps to the creation of the
libfprint device object, not to each open: fprintd opens and closes the
device for every client operation, while the Windows service keeps its unit
active, so a finger left on the sensor after a match is not read again by
the next client. (fprintd exits when idle for a while; the next start
creates the object again.) An enrollment that ends without a print
(cancel, error) counts as a discard. The capture action is not an engine
path; it clears the flag at every finger, so repeated captures never read
one placement twice. Not implemented: engine info 4 after a match (end the
service's capture loop with an empty sample while the session is locked),
which has no libfprint counterpart, and the flag writes on Windows power
events.

## Traced command tables

The Linux driver keeps the traced sequences as command tables:

| Tables | When |
|---|---|
| `activation_wake_cmds`, `activation_regs_cmds` | initialization steps 2 to 5, after a USB reset |
| `attempt_start_cmds`, `attempt_start_identify_cmds` | right after the initialization, or when the lift-wait flag is set: pin 1, GPIOs `0xfe`/`0xff` (identify variant only), detection steps 1 to 4 |
| `lift_start_cmds` or `lift_start_identify_cmds`, `lift_arm_cmds`, `irq_read_cmds`, `lift_restart_cmds` | waiting for the lift, before a detection unless the lift-wait flag is set |
| `detect_arm_cmds` | after the lift: detection steps 1 to 4 |
| `irq_read_cmds`, `detect_rearm_cmds`, `zones_read_cmds`, `detect_stop_cmds` | detection steps 6 to 8 |
| `capture_mode_cmds` | capture step 1 |
| `capture_setting_cmds[group][setting]` | before each image: the `0x5d`/`0x51` pair of "Capture settings" |
| `capture_trigger_cmds` | trigger, clear the interrupt, start the image read |
| `capture_frame_end_cmds` | after each image: clear the interrupt, two `0x3e` reads |
| `capture_power_off_cmds` | after the last image of the touch |
| `close_cmds` | closing the device: the release of "Session" |

Their contents match the Windows initialization, detection and capture
sequences byte for byte: run in the order of the Windows single unlock trace
(initialization to power off, 159 transfers) and of the failed-attempt trace
from a rejected touch through the lift wait to the next arm (147 transfers),
they give the same transfers, in the same order. The `0x20` reads that wait
for a state repeat only when the first read does not show it, so the traced
case sends the same bytes. Changing the tables needs evidence recorded here,
not a plausible-looking guess.

## Descriptors: `2df0:0003` versus `2df0:0007`

`2df0:0003` dumped from a Galaxy Book3 360 (2026-09-08). `2df0:0007` from a
Galaxy Book5 360, archived in the
[linux-fingerprint-drivers catalogue](https://github.com/jedbillyb/linux-fingerprint-drivers/blob/master/devices/2df0%3A0003/2df0-0007-lsusb.txt).

| Property | `2df0:0003` (this driver) | `2df0:0007` (not supported) |
|---|---|---|
| `bcdUSB` | `2.00` | `2.01` |
| `bcdDevice` | `1.27` | `f0.42` |
| Manufacturer / product strings | `Generic` / `CanvasBio CB2000` | `CanvasBio` / `CanvasBio CB2000` |
| `bmAttributes` | `0xe0`: self powered, remote wakeup | `0xa0`: remote wakeup |
| Interface | class `0xFF`, subclass `2` | class `0xFF`, subclass `2` |
| Endpoints | `0x01` bulk out, `0x82` bulk in, `0x83` interrupt in (16 bytes) | the same plus `0x84` interrupt in (16 bytes) |
| BOS descriptor | none, `GET_DESCRIPTOR(BOS)` stalls | Microsoft OS 2.0 platform capability (vendor code `0x15`) and an undecoded capability of type `0x11` |
| Microsoft OS 2.0 descriptor set | none, request `0x15` times out | 432 bytes, compatible ID `WINUSB` |
| First wake command (`0xDB`) | accepted | stalls |

The driver never reads `0x83`; the Windows traces show no traffic on it.

`2df0:0007` is a Realtek match-on-chip sensor that uses SDCP: its Windows
package names `RtsMocWbdi` and contains SDCP client functions. It speaks a
different protocol and is out of scope for this driver. If that is the sensor
in your machine, the place to look is the
[linux-fingerprint-drivers catalogue](https://github.com/jedbillyb/linux-fingerprint-drivers),
which tracks the device and its status; libfprint's own `realtek` driver is
where match-on-chip support belongs.
