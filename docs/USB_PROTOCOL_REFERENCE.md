# CB2000 USB Protocol Reference

This document keeps only the USB/protocol details that remain directly useful
for understanding the current public driver.

It is not a claim that every historical Windows parity note is still an exact,
byte-for-byte description of the current Linux snapshot. The source of truth for
current behavior is `src/canvasbio_cb2000.c`.

## Device Identity

| Property | Value |
|----------|-------|
| Vendor ID | `0x2DF0` |
| Product IDs | `0x0003`, `0x0007` |
| Image size | `80x64` grayscale |
| Image payload | `5120` bytes |
| Architecture | host-driven capture and matching |

## USB Request Families

| Request | Code | Purpose |
|---------|------|---------|
| `REQ_CONFIG` | `0xCA` | register read/write style exchanges |
| `REQ_STATUS` | `0xCC` | status query |
| `REQ_POLL` | `0xDA` | finger-detect polling |
| `REQ_INIT` | `0xDB` | mode set/reset |

| Direction | Meaning |
|-----------|---------|
| `0x40` | control out |
| `0xC0` | control in |
| `EP 0x01` | bulk out |
| `EP 0x82` | bulk in |

## Stable Command Primitives

These command roles are still reflected in the public driver and its command
tables:

| Function | Command shape |
|----------|---------------|
| enable finger-detect mode | `REQ_INIT val=0x0007 idx=0` |
| deactivate | `REQ_INIT val=0x0001 idx=0` |
| factory reset | `REQ_INIT val=0x0001 idx=1` |
| poll finger state | `REQ_POLL val=0x0007 idx=0` |
| register write | `REQ_CONFIG val=0x0002 ...` |
| register read | `REQ_CONFIG val=0x0003 ...` |

## Current Linux State Machine

The current driver exposes these important states in its cycle SSM:

- `CYCLE_WAIT_FINGER`
- `CYCLE_CAPTURE_START`
- `CYCLE_READ_IMAGE`
- `CYCLE_VERIFY_RESULT_STATUS`
- `CYCLE_VERIFY_READY_QUERY_A`
- `CYCLE_VERIFY_READY_QUERY_B`
- `CYCLE_VERIFY_READY_DECIDE`
- `CYCLE_SUBMIT_IMAGE`
- `CYCLE_WAIT_REMOVAL`

Important practical summary:

- the driver polls for finger presence
- starts capture through command sequences
- reads one `80x64` image via bulk transfers
- routes verify behavior through finalize-ACK and ready-query logic
- submits the image to host matching
- waits for removal before the next relevant cycle step

## Image Read Shape

The current image read path still uses:

- `20` bulk-in chunks
- `256` payload bytes per chunk
- total `5120` image bytes

That is the stable transport fact that matters most for tooling and protocol
inspection.

## Finalize-ACK Routing

Verify routing uses the finalize ACK pair plus conservative classification.

Current high-level rules:

- incomplete ACK data does not become a hard verdict
- mismatched ACK pairs are treated conservatively as retry
- retry-class status/result pairs trigger retry routing
- ready-class pairs allow the matcher flow to proceed
- possible device no-match classification is recognized separately

This logic lives in:

- `cb2000_classify_finalize_ack()`
- `cb2000_verify_finalize_ack_gate()`

## Retry-Class Status Pairs Still Recognized

The current code explicitly treats these status/result pairs as retry-class
signals:

- `00:00`
- `0f:00`
- `ff:0c`
- `f0:0f`
- `f3:0f`
- `88:08`
- `11:01`
- `08:00`
- `20:0f`
- `00:08`
- `11:00`
- `30:0f`

Some of them are further mapped to user-facing hints such as:

- move slightly up
- move slightly left
- move slightly right
- remove finger
- clean sensor
- try a different finger

## Ready-Query Routing

The current verify flow still includes complementary ready queries:

- `verify_ready_query_a_cmds`
- `verify_ready_query_b_cmds`

These do not perform matching. They only help decide whether the driver should:

- proceed toward host matching
- treat the capture as retry-class
- short-circuit a no-match-like device outcome when appropriate

## Command Tables That Still Matter

The current public driver still defines and uses these command tables:

- `activation_wake_cmds`
- `rearm_cmds`
- `capture_start_cmds`
- `capture_finalize_cmds`
- `verify_finalize_cmds`
- `verify_ready_query_a_cmds`
- `verify_ready_query_b_cmds`

These are the right protocol anchors for current maintenance work.

## What Was Removed From This Reference

Older notes that tried to describe the full protocol as an exact "six-phase
Windows parity script" were removed here.

Reason:

- they mixed historical reverse-engineering notes with current runtime behavior
- several of those notes were too absolute for the current public snapshot
- the current driver should be documented from the code outward, not from old
  parity prose inward
