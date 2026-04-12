# canvasbio-cb2000

Standalone libfprint driver package for the **CanvasBio CB2000** USB
fingerprint reader, implementing Match-on-Host with SIFT feature mosaicking and
CLAHE preprocessing.

- **VID/PID**: 0x2DF0 / 0x0003 or 0x0007
- **Sensor**: 80×64 px, 340 DPI, 6.0×4.8 mm active area
- **Platform**: Linux, libfprint ≥1.94

---

## Status

**R2.5 — Functional Snapshot**

| Metric | Result |
|--------|--------|
| GAR (Genuine Accept Rate) | 100% |
| FAR (False Accept Rate) | 0% (other-person finger) |
| Enrollment stages | 15 (lift-and-shift) |
| Template format | v2 (SIFT mosaic, two-hop affine aligned) |

---

## Requirements

**Container build (recommended)**:
- [distrobox](https://github.com/89luca89/distrobox) with a dedicated lab root under `~/.ContainerConfigs/<DBX_NAME>`
- Default day-to-day tester container: `canvasbio` on Ubuntu 24.04
- The container must have: `g++`, `meson`, `ninja-build`, `libopencv-dev`, libfprint build deps

**Host build**:
- `g++`, `meson`, `ninja-build`, OpenCV 4.x (`libopencv-dev`)
- libfprint source tree configured and built at a known path

**Analysis scripts**:
- Python ≥3.10

---

## Tested On

Primary validated development workflow:

- **Host**: Fedora Kinoite / Fedora Atomic KDE host
- **Build/runtime environment**: `distrobox` container based on **Ubuntu 24.04**
- **libfprint integration model**: local libfprint tree rebuilt with the custom
  `canvasbio_cb2000` driver enabled
- **Sensor IDs validated in the code/docs**:
  - `0x2DF0:0x0003`
  - `0x2DF0:0x0007`

Validation evidence in the project history includes:

- `15/15 MATCH`, `0/10 MATCH` for a wrong finger in the RC1 parity summary
- later R2.5 notes documenting `GAR=100%`, `FAR=0%` in limited controlled testing

This does **not** yet mean broad statistical validation across many users,
machines, or desktop environments. This public tree should be read as a working
snapshot, not as a fully supported rollout.

---

## Audience Split

- Want to install published packages:
  see `../releases/README.md`
- Want the driver rationale:
  see `DRIVER_NOTES.md`
- Want the development lab and helper scripts:
  see `DEVELOPER_LAB.md` and `../scripts/README.md`

## Quick Start

```bash
git clone https://github.com/kpagnussat/canvasbio-cb2000
cd canvasbio-cb2000

# Interactive orchestrator for development
bash ./scripts/cb2000_orchestrator.sh

# Low-friction Ubuntu tester path
bash ./scripts/cb2000_ubuntu_tester.sh prepare-host
bash ./scripts/cb2000_ubuntu_tester.sh prepare-runtime
bash ./scripts/cb2000_ubuntu_tester.sh test
bash ./scripts/cb2000_ubuntu_tester.sh build-deb
```

The orchestrator saves configuration in `.cb2000_env` (excluded from git) and
resolves the default container libfprint root to
`~/.ContainerConfigs/<DBX_NAME>/libfprint`.

Important: the current active path uses **15 lift-and-shift enrollment
captures**. If you still remember an older 5-scan prototype, that is obsolete.

Also note that the driver disables libfprint's generic thermal throttling model
for this device to avoid false hot-shutdown interruptions during valid enroll
sessions. See `DRIVER_NOTES.md`.

---

## Repository Structure

```
canvasbio-cb2000/
├── src/                        # Driver source
│   ├── canvasbio_cb2000.c      # Main libfprint driver (FpDevice)
│   ├── cb2000_sigfm_matcher.c  # SIGFM matching core (C)
│   ├── cb2000_sigfm_matcher.h
│   ├── cb2000_sigfm_opencv_helper.cpp  # OpenCV mosaic build/match
│   └── meson.build
├── tools/
│   └── 99-canvasbio.rules      # Optional udev rule for device permissions
├── scripts/                    # Build, session, analysis, and packaging scripts
│   ├── cb2000_orchestrator.sh          # Interactive main menu
│   ├── setup-libfprint.sh              # Integrate driver into a local libfprint tree
│   ├── build_sigfm_opencv_helper.sh    # Build OpenCV sidecar (.so)
│   ├── run_cb2000_interactive.sh       # Interactive enroll + verify session
│   ├── run_cb2000_test.sh              # Non-interactive test run
│   ├── battery_report.sh               # GAR/FAR session report
│   ├── gate_summary.sh                 # Enrollment gate analysis
│   ├── verify_metrics.sh               # Detailed verify metrics
│   ├── batch_summary.sh                # Batch verify summary
│   ├── build_libfprint_rpm_fedora.sh   # Package for Fedora / Kinoite / Silverblue
│   ├── build_libfprint_deb_ubuntu.sh   # Package for Ubuntu / Debian
│   ├── build_libfprint_pkg_arch.sh     # Package for Arch Linux
│   ├── build_libfprint_rpm_opensuse.sh # Package for openSUSE / Aeon / MicroOS
│   ├── requirements.txt                # Python deps for script-side helpers
│   └── README.md                       # Full script documentation
├── docs/
│   ├── FINDINGS.md             # Technical findings (hardware, SIGFM, gates, mosaic)
│   ├── DECISIONS.md            # Architecture decisions and rationale
│   └── CHANGELOG.md            # Version history with GAR/FAR per release
└── LICENSE                     # LGPL-2.1-or-later
```

Logs and sessions are stored **outside the repo** in `../dev_logs/sessions/`
(or the path configured via `CB2000_LOG_ROOT`).

---

## Scripts

See [`scripts/README.md`](scripts/README.md) for full documentation of every script,
environment variables, and example invocations.

---

## Packaging

Four hardened scripts build a distro-native package from the local libfprint
source tree. Each script auto-creates a named distrobox container on first run
and emits a ready-to-use install command at the end. For Fedora/openSUSE, the
install tip already uses the host-visible package path under
`HOST_ISOLATED_HOME` instead of the in-container path.

| Target | Script | Default container | Package |
|--------|--------|-------------------|---------|
| Fedora 43 / Kinoite / Silverblue | `build_libfprint_rpm_fedora.sh` | `cb2000-fedora` | `.rpm` |
| Ubuntu 24.04 / Debian | `build_libfprint_deb_ubuntu.sh` | `cb2000-ubuntu` | `.deb` |
| Arch Linux | `build_libfprint_pkg_arch.sh` | `cb2000-arch` | `.pkg.tar.zst` |
| openSUSE Tumbleweed / Aeon / MicroOS | `build_libfprint_rpm_opensuse.sh` | `cb2000-suse` | `.rpm` |

All scripts share the same interface:

```bash
# Default — persistent container (created automatically if needed)
./scripts/build_libfprint_rpm_fedora.sh

# Directly on the host, without distrobox
./scripts/build_libfprint_deb_ubuntu.sh --no-container

# Throwaway container — no persistent state
./scripts/build_libfprint_deb_ubuntu.sh --ephemeral

# Pre-create the container without running the build
./scripts/build_libfprint_pkg_arch.sh --create-container

# Already inside the correct container
./scripts/build_libfprint_rpm_opensuse.sh --in-container
```

Environment variables common to all packaging scripts:

| Variable | Default | Description |
|----------|---------|-------------|
| `LIBFPRINT_DIR` | `$HOME/libfprint` | Local libfprint source tree to package |
| `DBX_NAME` | per-script default | Distrobox container name |
| `CONTAINER_IMAGE` | per-script default | OCI image used to create the container |
| `HOST_ISOLATED_HOME` | `~/.ContainerConfigs/$DBX_NAME` | Host path used in atomic-desktop install tips (Fedora/openSUSE only) |

---

## How It Works

1. **Capture and preprocessing**: The driver captures raw 80×64 grayscale frames,
   applies background subtraction, then CLAHE + min/max normalization before
   feature extraction. This is necessary because the sensor is small and local
   contrast varies a lot across the finger area.

2. **Enrollment as coverage sampling**: The user performs **15 lift-and-shift
   captures**. Each accepted capture should contribute a slightly different
   view of the same finger: small position shifts, small rotations, and different
   coverage islands.

3. **Feature mosaic build**: The helper extracts SIFT keypoints/descriptors from
   each enrollment capture. The first capture becomes the reference frame. Other
   captures are aligned into that frame with:
   - BFMatcher + Lowe ratio test
   - affine transform estimation via `estimateAffine2D`
   - RANSAC outlier rejection

   If one capture cannot align directly to the first one, the helper tries a
   **two-hop affine alignment** through another already aligned enrollment image.
   After alignment, keypoints are reprojected into the same coordinate space and
   near-duplicates are removed.

   This is an actual **mosaic**, but not a stitched bitmap panorama. The result
   is a **master feature map**: a merged set of fingerprint keypoints/descriptors
   representing a larger effective finger coverage area than any single 80×64
   image provides.

4. **Template packing**: The v2 template stores both:
   - the 15 raw enrollment captures
   - the aggregated mosaic keypoints/descriptors

5. **Verification**: The probe capture goes through the same CLAHE preprocessing,
   SIFT extraction, ratio-test matching against the master mosaic descriptors,
   then affine geometric verification with RANSAC. If the probe is detected as
   blank, the driver performs one silent retry before returning `NO MATCH`.

6. **Fallback compatibility**: Older v1 prints remain supported. When the mosaic
   path is inconclusive, the driver can still fall back to gallery-style matching
   against individual enrollment images.

---

## Build Model

This repository uses two complementary build paths:

- `src/canvasbio_cb2000.c`
  - the main libfprint driver translation unit
  - it directly includes `cb2000_sigfm_matcher.c`
- `src/cb2000_sigfm_opencv_helper.cpp`
  - the OpenCV helper that builds and matches the feature mosaic
  - it is compiled separately into `build/libcb2000_sigfm_opencv.so`
    by `scripts/build_sigfm_opencv_helper.sh`

So the libfprint Meson integration only needs to register the main driver C file.
The OpenCV helper is a sidecar runtime component used by the v2 mosaic path.

---

## Optional udev Rule

If your system does not expose the CB2000 device with permissions suitable for
testing, install the provided rule:

```bash
sudo cp tools/99-canvasbio.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

The shipped rule covers both known product IDs:
- `0x2DF0:0x0003`
- `0x2DF0:0x0007`

Depending on your distro, you may need to adjust the target group (`plugdev`,
`uaccess`, or a local equivalent).

The operational shortcut for this step is:

```bash
bash ./scripts/cb2000_usb_host_access.sh --ensure
```

The current runners can also auto-attempt this fix when called with
`CB2000_AUTO_FIX_USB=1`.

---

## Known Limitations

- **Validation scale is still limited**: the best reported R2.5 results are
  strong, but they come from controlled batteries, not a broad multi-user study.
- **Very small sensor**: 80×64 / 340 DPI is hostile to classic minutiae pipelines.
  This project works around that with SIFT-based feature mosaicking, but the
  sensor remains physically constrained.
- **Feature mosaic, not pixel stitching**: the driver builds a master keypoint
  map, not a visual stitched fingerprint image. That is intentional.
- **libfprint integration is still custom**: this is not an upstreamed stock
  driver package yet.
- **The OpenCV helper is a required runtime dependency in the current
  snapshot**: the v2 mosaic path depends on `libcb2000_sigfm_opencv.so` being
  built and available.
- **Same-person different-finger testing is still thinner than ideal**: the
  project history documents strong wrong-finger rejection in controlled runs,
  but not a large formal impostor dataset.

---

## License and Author

**Project**: `canvasbio-cb2000`
**Author**: Kristofer Pagnussat
**License**: [LGPL-2.1-or-later](LICENSE)

---

## Third-Party Components

- **SIGFM**: Scale-Invariant Feature-based Fingerprint Matching algorithm.
  C implementation derived from the upstream C++ reference.
- **libfprint**: The GNOME Project. LGPL-2.1-or-later.
  https://gitlab.freedesktop.org/libfprint/libfprint
- **OpenCV**: Open Source Computer Vision Library. Apache 2.0 / BSD.
  https://opencv.org
