#!/usr/bin/env python3
"""
CB2000 verify false-negative root-cause summary.

Focus:
- classify correct-finger misses into actionable buckets;
- expose per-cycle per-gallery score shape;
- keep output compact enough for fast tuning loops.
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import statistics
import sys
from datetime import datetime
from typing import Any

from cb2000_paths import DEFAULT_RUNS_ROOT
from cb2000_run_report import read_verify_lines, resolve_run_dir, extract_cycles

DEFAULT_LOGS_ROOT = DEFAULT_RUNS_ROOT


def derive_correct_count(cycles: list[dict[str, Any]],
                         fixed: int | None,
                         cutoff_time: str | None) -> int | None:
    if fixed is not None:
        return fixed
    if cutoff_time is None:
        return None

    for i, cycle in enumerate(cycles):
        if cycle["ts"] == cutoff_time:
            return i + 1
    return None


def classify_fn(cycle: dict[str, Any], near_min: float, eps: float = 1e-9) -> tuple[str, int, float]:
    all_sigfm = cycle.get("all_sigfm", {}) or {}
    if not all_sigfm:
        return ("no_telemetry", 0, 0.0)

    scores = [vals[0] for vals in all_sigfm.values()]
    nonzero = sum(1 for s in scores if s > eps)
    best = max(scores) if scores else 0.0

    if best <= eps:
        return ("all_zero", nonzero, best)
    if best >= near_min:
        return ("near_match", nonzero, best)
    if nonzero <= 2:
        return ("low_signal_sparse", nonzero, best)
    return ("low_signal_diffuse", nonzero, best)


def top_gallery_triplet(cycle: dict[str, Any]) -> list[tuple[int, float]]:
    all_sigfm = cycle.get("all_sigfm", {}) or {}
    ranked = sorted(((g, vals[0]) for g, vals in all_sigfm.items()),
                    key=lambda x: x[1], reverse=True)
    return ranked[:3]


def format_report(run_dir: pathlib.Path,
                  cycles: list[dict[str, Any]],
                  correct_count: int | None,
                  near_min: float) -> str:
    out: list[str] = []
    w = out.append

    total = len(cycles)
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    if correct_count is None:
        correct_count = total
        split_note = "No split provided; assuming all cycles are correct-finger."
    elif not (0 < correct_count <= total):
        split_note = f"Invalid split ({correct_count}); assuming all cycles are correct-finger."
        correct_count = total
    else:
        split_note = f"Correct finger cycles: 1-{correct_count}; wrong finger: {correct_count + 1}-{total}."

    correct = cycles[:correct_count]
    misses: list[tuple[int, dict[str, Any], str, int, float]] = []

    for idx, cycle in enumerate(correct, 1):
        if cycle["decision"] == "MATCH":
            continue
        bucket, nonzero, best = classify_fn(cycle, near_min=near_min)
        misses.append((idx, cycle, bucket, nonzero, best))

    bucket_counts = collections.Counter(item[2] for item in misses)
    best_gallery_counts = collections.Counter(
        item[1]["sigfm_gal"] for item in misses if item[1]["sigfm_gal"] >= 0
    )
    miss_best_scores = [item[4] for item in misses]
    miss_nonzero = [item[3] for item in misses]

    w("CB2000 VERIFY FN ROOTCAUSE REPORT")
    w("=" * 90)
    w(f"Generated: {now}")
    w(f"Run dir: {run_dir}")
    w(f"Total cycles: {total}")
    w(split_note)
    w(f"Near-match threshold: best_sigfm >= {near_min:.3f}")
    w("")

    w("[SUMMARY]")
    w(f"- correct cycles: {len(correct)}")
    w(f"- correct MATCH: {sum(1 for c in correct if c['decision'] == 'MATCH')}")
    w(f"- correct misses (FN+RETRY): {len(misses)}")
    if len(correct) > 0:
        w(f"- FN ratio: {100.0 * len(misses) / len(correct):.1f}%")
    w("")

    w("[ROOTCAUSE BUCKETS]")
    if not misses:
        w("- none")
    else:
        for bucket, count in bucket_counts.most_common():
            w(f"- {bucket}: {count}")
    w("")

    w("[MISS SCORE SHAPE]")
    if not misses:
        w("- none")
    else:
        w(f"- best_sigfm: min={min(miss_best_scores):.4f} "
          f"p50={statistics.median(miss_best_scores):.4f} "
          f"max={max(miss_best_scores):.4f}")
        w(f"- nonzero galleries: min={min(miss_nonzero)} "
          f"p50={statistics.median(miss_nonzero):.1f} "
          f"max={max(miss_nonzero)}")
    w("")

    w("[MISS BEST GALLERY]")
    if not best_gallery_counts:
        w("- none")
    else:
        total_miss_with_gallery = sum(best_gallery_counts.values())
        for gal, count in best_gallery_counts.most_common():
            pct = 100.0 * count / total_miss_with_gallery
            w(f"- g{gal}: {count} ({pct:.1f}%)")
    w("")

    w("[PER-MISS DETAIL]")
    if not misses:
        w("- none")
    else:
        for idx, cycle, bucket, nonzero, best in misses:
            top3 = top_gallery_triplet(cycle)
            top3_txt = ", ".join(f"g{g}:{s:.3f}" for g, s in top3) if top3 else "none"
            w(
                f"- cycle={idx:02d} ts={cycle['ts']} decision={cycle['decision']} "
                f"bucket={bucket} best={best:.4f} best_g={cycle['sigfm_gal']} "
                f"nonzero={nonzero} top3=[{top3_txt}]"
            )
    w("")

    w("[NOTES]")
    w("- `all_zero` dominant => probe/template mismatch severe (likely representativity gap).")
    w("- `near_match` dominant => classifier/margin likely too hard.")
    w("- `low_signal_*` dominant => matcher sees weak correspondences; inspect enroll samples per stage.")

    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze verify false-negative root causes for a run")
    ap.add_argument("--run", default=None,
                    help="Run directory (absolute path) or run ID under the current run root")
    ap.add_argument("--logs-root", type=pathlib.Path, default=DEFAULT_LOGS_ROOT,
                    help=f"Run root directory (default: {DEFAULT_LOGS_ROOT})")
    ap.add_argument("--correct-count", type=int, default=None,
                    help="Number of initial cycles from correct finger")
    ap.add_argument("--cutoff-time", default=None,
                    help="Timestamp of last correct-finger verify (HH:MM:SS.mmm)")
    ap.add_argument("--near-min", type=float, default=0.30,
                    help="Threshold to classify miss as near-match (default: 0.30)")
    ap.add_argument("--no-save", action="store_true",
                    help="Print report only; do not save")
    ap.add_argument("--out", type=pathlib.Path, default=None,
                    help="Optional explicit output file path")
    args = ap.parse_args()

    try:
        run_dir = resolve_run_dir(args.run, args.logs_root)
    except FileNotFoundError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if not run_dir.exists():
        print(f"ERROR: run dir not found: {run_dir}", file=sys.stderr)
        return 1

    lines = read_verify_lines(run_dir)
    if not lines:
        print(f"ERROR: no verify logs found under {run_dir}", file=sys.stderr)
        return 1

    cycles = extract_cycles(lines)
    if not cycles:
        print(f"ERROR: no verify decisions parsed in {run_dir}", file=sys.stderr)
        return 1

    correct_count = derive_correct_count(cycles, args.correct_count, args.cutoff_time)
    if args.cutoff_time and correct_count is None:
        print(f"WARNING: cutoff time '{args.cutoff_time}' not found; using all cycles as correct-finger.",
              file=sys.stderr)

    report = format_report(run_dir, cycles, correct_count, near_min=args.near_min)
    print(report)

    if args.no_save:
        return 0

    out_path = args.out if args.out else (run_dir / "verify_fn_rootcause.txt")
    try:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(report + "\n", encoding="utf-8")
        print(f"\nReport saved to: {out_path}")
    except OSError as exc:
        print(f"\nWARN: could not save report to {out_path}: {exc}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
