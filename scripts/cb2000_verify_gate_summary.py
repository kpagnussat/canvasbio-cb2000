#!/usr/bin/env python3
"""
Summarize CB2000 verify gate telemetry from runtime logs.

Default runs root:
  $CB2000_RUNTIME_ROOT/runs when available, otherwise $CB2000_LOG_ROOT or
  ../dev_logs/sessions

Usage:
  cb2000_verify_gate_summary.py
  cb2000_verify_gate_summary.py --run /path/to/run_dir
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import sys

from cb2000_paths import DEFAULT_RUNS_ROOT


RE_GATE = re.compile(
    r"\[ VERIFY_GATE \] "
    r"ack1=(?P<ack1>[0-9a-f:]+) "
    r"ack2=(?P<ack2>[0-9a-f:]+) "
    r"decision=(?P<decision>[A-Z_]+) "
    r"result_class=(?P<result_class>[A-Z_]+) "
    r"status=(?P<status>0x[0-9a-f]+) "
    r"result=(?P<result>0x[0-9a-f]+) "
    r"mismatch=(?P<mismatch>\d+)"
)
RE_QUERY_A = re.compile(r"\[ VERIFY_READY_QUERY_A \].* class=(?P<class>[A-Z_]+)")
RE_QUERY_B = re.compile(r"\[ VERIFY_READY_QUERY_B \].* class=(?P<class>[A-Z_]+)")
RE_MATRIX = re.compile(r"\[ VERIFY_READY_MATRIX \].* => (?P<matrix>[A-Z_]+)")
RE_MATCH_DECISION = re.compile(
    r"\[\s*(?:SIGFM_MATCH|SIGFM_NCC|NCC)\s*\]\s+"
    r"(?:action=\S+\s+)?decision=(?P<decision>[A-Z_]+)\s*->\s*result=(?P<result>.+)$"
)
RE_UI_MATCH = re.compile(r"^MATCH!$")
RE_UI_NOMATCH = re.compile(r"^NO MATCH!$")


def pick_latest_run(runs_root: pathlib.Path) -> pathlib.Path:
    if not runs_root.exists():
        raise FileNotFoundError(f"runs root not found: {runs_root}")
    dirs = sorted([p for p in runs_root.iterdir() if p.is_dir()], key=lambda p: p.name)
    if not dirs:
        raise FileNotFoundError(f"no run dirs under: {runs_root}")
    return dirs[-1]


def read_lines(paths: list[pathlib.Path]) -> list[str]:
    lines: list[str] = []
    for p in paths:
        try:
            lines.extend(p.read_text(encoding="utf-8", errors="replace").splitlines())
        except OSError:
            continue
    return lines


def pick_verify_log_sources(run_dir: pathlib.Path) -> list[pathlib.Path]:
    # Prefer the consolidated log to avoid counting attempts twice.
    for name in ("verify_log.txt", "verify.log"):
        verify_log = run_dir / name
        if verify_log.exists():
            return [verify_log]

    verify_logs = sorted(run_dir.glob("verify_log*.txt"))
    if verify_logs:
        return verify_logs

    return sorted((run_dir / "attempts").glob("verify_attempt*.log"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", type=pathlib.Path, default=None, help="specific run directory")
    ap.add_argument(
        "--runs-root",
        type=pathlib.Path,
        default=DEFAULT_RUNS_ROOT,
        help="runs root when --run is not provided",
    )
    args = ap.parse_args()

    run_dir = args.run if args.run else pick_latest_run(args.runs_root)
    if not run_dir.exists():
        print(f"ERROR: run dir not found: {run_dir}", file=sys.stderr)
        return 1

    log_sources = pick_verify_log_sources(run_dir)
    lines = read_lines(log_sources)

    if not lines:
        print(f"ERROR: no verify logs found under {run_dir}", file=sys.stderr)
        return 1

    gate_pairs = collections.Counter()
    gate_decisions = collections.Counter()
    gate_classes = collections.Counter()
    gate_status_pairs = collections.Counter()
    query_a = collections.Counter()
    query_b = collections.Counter()
    matrix_counts = collections.Counter()
    match_counts = collections.Counter()
    ui_counts = collections.Counter()

    for line in lines:
        m = RE_GATE.search(line)
        if m:
            ack_pair = f"{m.group('ack1')} | {m.group('ack2')}"
            gate_pairs[ack_pair] += 1
            gate_decisions[m.group("decision")] += 1
            gate_classes[m.group("result_class")] += 1
            gate_status_pairs[f"{m.group('status')}:{m.group('result')}"] += 1
            continue

        m = RE_QUERY_A.search(line)
        if m:
            query_a[m.group("class")] += 1
            continue

        m = RE_QUERY_B.search(line)
        if m:
            query_b[m.group("class")] += 1
            continue

        m = RE_MATRIX.search(line)
        if m:
            matrix_counts[m.group("matrix")] += 1
            continue

        m = RE_MATCH_DECISION.search(line)
        if m:
            match_counts[f"{m.group('decision')} -> {m.group('result')}"] += 1
            continue

        if RE_UI_MATCH.search(line):
            ui_counts["MATCH!"] += 1
            continue
        if RE_UI_NOMATCH.search(line):
            ui_counts["NO MATCH!"] += 1
            continue

    print(f"run_dir={run_dir}")
    print()

    def dump_counter(title: str, counter: collections.Counter) -> None:
        print(f"[{title}]")
        if not counter:
            print("  (none)")
        else:
            for key, value in counter.most_common():
                print(f"  {value:4d}  {key}")
        print()

    dump_counter("GATE_ACK_PAIRS", gate_pairs)
    dump_counter("GATE_DECISION", gate_decisions)
    dump_counter("GATE_RESULT_CLASS", gate_classes)
    dump_counter("GATE_STATUS_RESULT", gate_status_pairs)
    dump_counter("READY_QUERY_A_CLASS", query_a)
    dump_counter("READY_QUERY_B_CLASS", query_b)
    dump_counter("READY_MATRIX", matrix_counts)
    dump_counter("MATCH_DECISION", match_counts)
    dump_counter("UI_RESULT", ui_counts)

    missing = []
    for required in ("READY", "RETRY", "DEVICE_NOMATCH", "UNKNOWN"):
        if gate_classes.get(required, 0) == 0:
            missing.append(required)
    print("[MATRIX_COVERAGE]")
    if missing:
        print("  Missing gate classes:", ", ".join(missing))
    else:
        print("  All gate classes observed.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
