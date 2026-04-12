# CB2000 USB Protocol Reference

**Project location:** `/home/kris/Data/Estudo-Conhecimento/Projetos/CanvasMancer`

Complete USB protocol specification derived from Windows PCAPs and Ghidra RE.

**Updated:** 2026-02-24
**PCAP source:** `archive/legacy/reference/PCAPs/Windows/*TRASHOUT.json`
**RE source:** `reference/Reverse_Engineering-Ghidra/`

---

## Device Identity

| Property | Value |
|----------|-------|
| Vendor ID | 0x2DF0 (CanvasBio) |
| Product IDs | 0x0003, 0x0007 |
| Image | 80x64 grayscale, 5120 bytes |
| Resolution | 13.39 ppmm (~340 DPI) |
| Template size | 40968 bytes (0xA008 = 5123 x 8) |
| Bulk chunk size | 320 bytes (64 header + 256 payload) |
| Architecture | Match-on-Host (MoH) |

---

## USB Request Types

| Request | Code (hex) | Code (dec) | Purpose |
|---------|-----------|------------|---------|
| REQ_CONFIG | 0xCA | 202 | Register read/write |
| REQ_STATUS | 0xCC | 204 | Status query |
| REQ_POLL | 0xDA | 218 | Finger detection poll |
| REQ_INIT | 0xDB | 219 | Mode set/reset |

| Direction | bmRequestType | Description |
|-----------|--------------|-------------|
| CTRL_OUT | 0x40 | Host -> Device (write register) |
| CTRL_IN | 0xC0 | Device -> Host (read register) |
| BULK_OUT | EP 0x01 | Bulk data to device |
| BULK_IN | EP 0x82 | Bulk data from device |

---

## USB Register Map

From RE of `FUN_180036900` (driver.dll) and PCAP evidence.

### Sensor Configuration

| Register | Value | USB Bytes | Purpose |
|----------|-------|-----------|---------|
| 0x03 | 0x0000 | `a9:03:00:00` | Reset / mode clear |
| 0x04 | 0x0000 | `a9:04:00:00` | Stop / idle mode |
| 0x09 | 0x0000 | `a9:09:00:00` | Trigger / start command |
| 0x0c | 0x0000 | `a9:0c:00` | Read start |
| 0x0d | 0x0000 | `a9:0d:00` | Cleanup / post-capture reset |
| 0x10 | 0x6000 | `a9:10:60:00` | Capture mode (standard) |
| 0x10 | 0x0001 | `a9:10:00:01` | Capture mode (alternate, post-detect) |
| 0x20 | 0x0000 | `a8:20:00:00` | State read (returns `ff:00:01:xx`) |
| 0x26 | 0x3000 | `a9:26:30:00` | Capture config (standard) |
| 0x26 | 0x0000 | `a9:26:00:00` | Capture config (alternate) |
| 0x2f | 0xf6ff | `a9:2f:f6:ff` | Capture filter/gain |
| 0x38 | 0x0100 | `a9:38:01:00` | Image config enable |
| 0x3b | 0x1400 | `a9:3b:14:00` | Image size = 0x1400 = 5120 bytes |
| 0x3d | 0xff0f | `a9:3d:ff:0f` | Quality threshold |
| 0x3e | 0x0000 | `a8:3e:00:00` | Quality/status read (4B response) |
| 0x50 | 0x1200 | `a9:50:12:00` | AFE gain setting |

### Calibration Registers

| Register | Value | USB Bytes | Context | Purpose |
|----------|-------|-----------|---------|---------|
| 0x5d | 0x3d00 | `a9:5d:3d:00` | Detection (Phase 1) | Sensitivity |
| 0x5d | 0x4d00 | `a9:5d:4d:00` | Capture (Phase 4) | Sensitivity |
| 0x51 | 0xa801 | `a9:51:a8:01` | Detection (standard) | Threshold |
| 0x51 | 0x6801 | `a9:51:68:01` | Detection (alternate) | Threshold |
| 0x51 | 0x8801 | `a9:51:88:01` | Capture (Phase 4) | Threshold |

---

## USB Command High-Level Map

| Command | Request | Value | Index | Description |
|---------|---------|-------|-------|-------------|
| SetMode FINGER_DOWN | REQ_INIT | 0x0007 | 0 | Enable detection mode |
| Deactivate | REQ_INIT | 0x0001 | 0 | End session |
| FactoryReset | REQ_INIT | 0x0001 | 1 | Full reconfig required after |
| WaitFingerDown | REQ_POLL | 0x0007 | 0 | byte[1]=0x00 (no), 0x01 (present) |
| WaitFingerUp | REQ_POLL | 0x0007 | 0 | byte[1]=0x00 (removed) |
| WriteRegister | REQ_CONFIG | 0x0002 | varies | `a9:<reg>:<val_lo>:<val_hi>` |
| ReadRegister | REQ_CONFIG | 0x0003 | varies | `a8:<reg>:<val_lo>:<val_hi>` |
| CaptureConfig | REQ_CONFIG | 0x0003 | 5123 | idx=5123 = IMAGE_SIZE+3 |
| StatusQuery | REQ_STATUS | 0x0000 | 0 | 4-byte response |
| PollFE | REQ_POLL | 0x00FE | 0 | Init-phase poll |
| PollFF | REQ_POLL | 0x00FF | 0 | Init-phase poll |

---

## Six-Phase Verify Protocol

Every verify cycle follows this exact sequence. PCAP evidence from `ate512-desbloqueio-erros-1-2-3-4-5-6sucessoTRASHOUT.json` (28 cycles: 27 failures + 1 success).

### Phase 1: Wait for Finger Detection

**Windows RE:** `FUN_180036900`, `WbioQuerySensorInterface`
**Linux driver:** `CYCLE_WAIT_FINGER` -> `poll_finger_run_state()` L2758
**Command tables:** `capture_start_cmds[]` L811

| Step | Command | Response | Notes |
|------|---------|----------|-------|
| 1 | `REQ_INIT val=0x0007 idx=0` | - | SetMode FINGER_DOWN |
| 2 | `REQ_CONFIG idx=3` + `a9:04:00` | - | Setup stop/idle |
| 3 | `REQ_STATUS` | `00:00:00:00` | Confirm ready |
| 4 | Calibration: `a9:5d:3d:00`, `a9:51:a8:01` | - | Detection sensitivity + threshold |
| 5 | `REQ_POLL val=0x0007` (loop) | byte[1]=0x01 when finger present | ~16ms cadence, ~12s timeout |

### Phase 2: First Capture + Status Check

**Windows RE:** `FUN_18003110c` (double capture, address 18003110c)
**Linux driver:** `CYCLE_CAPTURE_START` L5337
**Command tables:** `capture_start_cmds[]` L811

| Step | Command | Response | Notes |
|------|---------|----------|-------|
| 1 | `REQ_CONFIG idx=3` + `a8:08:00` | BULK_IN 3B: `ff:00:XX` | Capture trigger |
| 2 | `a9:09:00:00` | - | Trigger |
| 3 | `REQ_STATUS` | `00:00:00:00` | Confirm |
| 4 | `REQ_CONFIG idx=4` + `a8:3e:00:00` | BULK_IN 4B: `ff:00:YY:ZZ` | **STATUS QUERY #1** (quality ACK) |

### Phase 3: Post-Quality Configuration

**Linux driver:** `CYCLE_VERIFY_READY_QUERY_A` L5502, `CYCLE_VERIFY_READY_QUERY_B` L5521

| Step | Command | Response | Notes |
|------|---------|----------|-------|
| 1 | `a9:03:00:00` | - | Mode clear |
| 2 | `REQ_CONFIG idx=4` + `a8:20:00:00` | BULK_IN 4B: `ff:00:01:07` | State acknowledgment |
| 3 | `a9:0d:00` | - | Cleanup |
| 4 | `a9:10:00:01` | - | Switch to alternate capture mode |
| 5 | `a9:26:00:00` | - | Alternate capture config |
| 6 | `a9:09:00:00` | - | Re-trigger |
| 7 | `a9:0c:00` | - | Read start |
| 8 | `REQ_CONFIG idx=4` + `a8:20:00:00` | BULK_IN 4B: `ff:00:01:02` | Ready for second capture |

### Phase 4: Calibration + Second Capture (IMAGE READ)

**Windows RE:** `FUN_18003110c` (second invocation with arg 5)
**Linux driver:** `CYCLE_READ_IMAGE` L5372 -> `image_read_run_state()` L3018

| Step | Command | Response | Notes |
|------|---------|----------|-------|
| 1 | `a9:5d:4d:00` | - | Capture calibration: sensitivity |
| 2 | `a9:51:88:01` | - | Capture calibration: threshold |
| 3 | `a9:04:00:00` | - | Stop/prepare |
| 4 | `a9:09:00:00` | - | Trigger |
| 5 | `REQ_CONFIG idx=5123 (0x1403)` | - | **Second capture trigger** (idx = IMAGE_SIZE+3) |
| 6 | `a8:06:00:00...` (259 bytes) | - | Image transfer start |
| 7 | 20x BULK_IN (320 bytes each) | Pixel data | 64B header + 256B payload per chunk |
| 8 | Interleaved 256B zero BULK_OUT | - | Padding |

**Image assembly:** 20 chunks x 256B payload = 5120 bytes -> 80x64 grayscale image.

**GAP:** Windows does TWO captures (arg 4 then arg 5). Linux currently does only the second capture.

### Phase 5: Second Status Check

**Linux driver:** `CYCLE_VERIFY_RESULT_STATUS` L5399

| Step | Command | Response | Notes |
|------|---------|----------|-------|
| 1 | `a9:09:00:00` | - | Trigger status |
| 2 | `REQ_CONFIG idx=4` + `a8:3e:00:00` | BULK_IN 4B: `ff:00:YY:ZZ` | **STATUS QUERY #4** (quality ACK) |
| 3 | `REQ_CONFIG idx=4` + `a8:3e:00:00` | BULK_IN 4B: `ff:00:YY:ZZ` | **STATUS QUERY #5** (duplicate!) |
| 4 | `a9:0d:00` | - | Cleanup |

Windows queries quality **twice** with identical responses. May be stability check.

### Phase 6: Decision (Host-Driven Only)

**Windows RE:** `FUN_18001df7c` (result handler), `FUN_18003a224` (proprietary matcher)
**Linux driver:** `CYCLE_VERIFY_READY_DECIDE` L5546 -> `CYCLE_SUBMIT_IMAGE` L5617

All matching happens in host software. Device NEVER decides.

| Outcome | USB Sequence | Host Action |
|---------|-------------|-------------|
| SUCCESS | `REQ_INIT val=0x0001 idx=0` (DEACTIVATE) -> END | Match found |
| FAILURE (retry) | `REQ_INIT val=0x0001 idx=0` -> 22s pause -> `REQ_INIT val=0x0001 idx=1` (FACTORY_RESET) | Up to 3 retries |
| FAILURE (immediate) | `REQ_INIT val=0x0001 idx=1` (FACTORY_RESET, no deactivate) | Device error |

**PCAP evidence:** Cycles 1 (fail) and 27 (success) produce **identical** Phases 1-5. Only Phase 6 differs.

---

## Quality ACK Codes

Status register 0x3e returns 4 bytes after capture. Linux driver classifies at `cb2000_classify_finalize_ack()` L2391.

| byte[2] | byte[3] | Meaning | Action |
|---------|---------|---------|--------|
| 0xff | 0x0f | Image quality GOOD | Proceed to host matching |
| 0x20 | 0x0f | Move finger up | Retry |
| 0x08 | 0x00 | Sensor issue | Retry |
| 0x00 | 0x08 | Clean sensor | Retry |
| 0x11 | 0x00 | Move right | Retry |
| 0x88 | 0x08 | Move left | Retry |
| 0x30 | 0x0f | Try different finger | Retry |

---

## PCAP Statistics (ate512 dataset, 28 cycles)

| Metric | Value |
|--------|-------|
| Total packets | 7494 |
| REQ_POLL (218) | 5836 |
| REQ_CONFIG (202) | 451 |
| REQ_STATUS (204) | 360 |
| REQ_INIT (219) | 39 |

---

## Linux Driver Command Tables

| Table | Line | Phase | Commands |
|-------|------|-------|----------|
| `activation_wake_cmds[]` | 631 | Init | Wake sequence |
| `activation_regs_cmds[]` | 647 | Init | Register configuration |
| `activation_trigger_cmds[]` | 695 | Init | Trigger activation |
| `activation_capture_cmds[]` | 707 | Init | First capture config |
| `activation_finalize_cmds[]` | 740 | Init | Finalize activation |
| `rearm_cmds[]` | 759 | Rearm | Soft rearm between cycles |
| `capture_start_cmds[]` | 811 | Capture | Main capture sequence |
| `verify_start_cmds[]` | 870 | Verify | Verify-specific start |
| `capture_finalize_cmds[]` | 924 | Finalize | Capture cleanup |
| `verify_finalize_cmds[]` | 944 | Finalize | Verify-specific cleanup |
| `verify_ready_query_a_cmds[]` | 616 | Verify | Ready query A |
| `verify_ready_query_b_cmds[]` | 621 | Verify | Ready query B |

---

## Parity Gap Analysis (Linux vs Windows)

| # | Gap | Severity | Driver Line(s) | Windows Behavior | Linux Status |
|---|-----|----------|----------------|------------------|--------------|
| 1 | SetMode FINGER_DOWN before capture | **CRITICAL** | L811, L870 | `REQ_INIT val=0x0007 idx=0` before every capture start | Missing from `capture_start_cmds[]` and `verify_start_cmds[]`. Present in `activation_finalize_cmds` (L751) and `rearm_cmds` (L800) but NOT repeated before capture. |
| 2 | Double capture (idx=5123) | **MAJOR** | L860, L918, L5372 | FUN_18003110c does TWO captures (arg 4 + arg 5), two 5120B buffers | Single capture only. Phase 2 quality probe exists (L812-820) but first full image read not implemented. |
| 3 | Short capture for verify (4 bursts) | **PLANNED** | L811-863, L870-921 | Windows uses optimized 4-burst sequence for verify/identify vs full activation | Full 52-command sequence for all actions. Historical plan archived under `archive/legacy/docs/non_core_20260411/plans/PLANO_PROXIMA_SESSAO_V80_SHORT_ARM.md`. |
| 4 | Wakeup delay runtime config | **LOW** | L98 | Implicit in Windows timings | Hardcoded 500ms. Plan: `CB2000_WAKEUP_DELAY_MS` env var. |
| 5 | Duplicate quality ACK queries | **NONE** | L928-933 | Two identical `a8:3e` queries | Already correctly implemented. |
| 6 | Activation sequence | **NONE** | L631-756 | Full register config per RE/PCAP | Correctly implemented, parity verified. |
| 7 | Rearm sequence | **NONE** | L759-805 | Deactivate + reconfig + SetMode FINGER_DOWN | Correctly implemented, parity verified. |

**Note**: "Parity" here means USB command sequences, timings, and byte-level payloads. The host-side matching algorithm (SIGFM-only in verify/identify) intentionally differs from Windows' proprietary matcher (FUN_18003a224).
