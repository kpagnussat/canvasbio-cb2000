#!/usr/bin/env python3
"""
Summarize verify outcomes for CB2000 (MATCH / NO_MATCH / RETRY).

By default it reads verify_log.txt from a run directory and reports:
- total attempts
- counts by decision
- rates (%)
- optional control-adjusted metrics excluding last N attempts

Usage:
  cb2000_verify_metrics.py
  cb2000_verify_metrics.py --run /path/to/run_dir
  cb2000_verify_metrics.py --run /path/to/run_dir --exclude-last 1
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import sys

from cb2000_paths import DEFAULT_RUNS_ROOT

RE_MATCH = re.compile(
    r"\[\s*(?:SIGFM_MATCH|SIGFM_NCC|NCC)\s*\]\s+"
    r"(?:action=\S+\s+)?decision=(MATCH|NO_MATCH|RETRY|ERROR).*?->\s*result="
)


def pick_latest_run(runs_root: pathlib.Path) -> pathlib.Path:
    if not runs_root.exists():
        raise FileNotFoundError(f"runs root not found: {runs_root}")
    dirs = sorted([p for p in runs_root.iterdir() if p.is_dir()], key=lambda p: p.name)
    if not dirs:
        raise FileNotFoundError(f"no run dirs under: {runs_root}")
    return dirs[-1]


def read_verify_lines(run_dir: pathlib.Path) -> list[str]:
    for name in ("verify_log.txt", "verify.log"):
        verify_log = run_dir / name
        if verify_log.exists():
            return verify_log.read_text(encoding="utf-8", errors="replace").splitlines()

    # Fallback for older runs without consolidated verify_log.txt.
    logs = sorted((run_dir / "attempts").glob("verify_attempt*.log"))
    lines: list[str] = []
    for p in logs:
        try:
            lines.extend(p.read_text(encoding="utf-8", errors="replace").splitlines())
        except OSError:
            continue
    return lines


def extract_match_decisions(lines: list[str]) -> list[str]:
    out: list[str] = []
    for line in lines:
        m = RE_MATCH.search(line)
        if m:
            out.append(m.group(1))
    return out


def pct(n: int, total: int) -> float:
    return (100.0 * n / total) if total else 0.0


def summarize(decisions: list[str]) -> tuple[int, collections.Counter[str], dict[str, float]]:
    counts = collections.Counter(decisions)
    total = len(decisions)
    rates = {
        "match_rate": pct(counts.get("MATCH", 0), total),
        "nomatch_rate": pct(counts.get("NO_MATCH", 0), total),
        "retry_rate": pct(counts.get("RETRY", 0), total),
        "error_rate": pct(counts.get("ERROR", 0), total),
    }
    return total, counts, rates


def print_summary(title: str, decisions: list[str]) -> None:
    total, counts, rates = summarize(decisions)
    print(f"[{title}]")
    print(f"  total={total}")
    print(f"  match={counts.get('MATCH', 0)} ({rates['match_rate']:.1f}%)")
    print(f"  no_match={counts.get('NO_MATCH', 0)} ({rates['nomatch_rate']:.1f}%)")
    print(f"  retry={counts.get('RETRY', 0)} ({rates['retry_rate']:.1f}%)")
    print(f"  error={counts.get('ERROR', 0)} ({rates['error_rate']:.1f}%)")
    print()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", type=pathlib.Path, default=None, help="specific run directory")
    ap.add_argument(
        "--runs-root",
        type=pathlib.Path,
        default=DEFAULT_RUNS_ROOT,
        help="runs root when --run is not provided",
    )
    ap.add_argument(
        "--exclude-last",
        type=int,
        default=0,
        help="exclude last N matcher decisions (e.g. wrong-finger control at end)",
    )
    args = ap.parse_args()

    run_dir = args.run if args.run else pick_latest_run(args.runs_root)
    if not run_dir.exists():
        print(f"ERROR: run dir not found: {run_dir}", file=sys.stderr)
        return 1

    lines = read_verify_lines(run_dir)
    decisions = extract_match_decisions(lines)
    if not decisions:
        print(f"ERROR: no matcher decisions found under {run_dir}", file=sys.stderr)
        return 1

    print(f"run_dir={run_dir}")
    print(f"last_decision={decisions[-1]}")
    print()
    print_summary("ALL_ATTEMPTS", decisions)

    if args.exclude_last > 0:
        if args.exclude_last >= len(decisions):
            print(
                f"ERROR: exclude-last ({args.exclude_last}) >= total decisions ({len(decisions)})",
                file=sys.stderr,
            )
            return 1
        trimmed = decisions[:-args.exclude_last]
        print_summary(f"EXCLUDING_LAST_{args.exclude_last}", trimmed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
