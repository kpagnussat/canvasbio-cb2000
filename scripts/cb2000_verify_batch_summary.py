#!/usr/bin/env python3
"""Batch summary for verify runs with peak-gallery concentration diagnostics.

This script reuses the same parsing logic from cb2000_run_report.py and prints
compact cross-run metrics for faster tuning loops.

Examples:
  python3 scripts/cb2000_verify_batch_summary.py --latest 3 \
      --logs-root ~/.ContainerConfigs/canvasbio/cb2000_runtime/runs

  python3 scripts/cb2000_verify_batch_summary.py \
      --runs 20260228_192613 20260228_193536 \
      --logs-root ~/.ContainerConfigs/canvasbio/cb2000_runtime/runs \
      --correct-count 16
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import sys
from typing import Any

from cb2000_paths import DEFAULT_RUNS_ROOT
from cb2000_run_report import extract_cycles, extract_gate_info, read_verify_lines


DEFAULT_LOGS_ROOT = DEFAULT_RUNS_ROOT


def _has_verify_log(run_dir: pathlib.Path) -> bool:
    return ((run_dir / "verify_log.txt").exists() or
            (run_dir / "verify.log").exists() or
            any(run_dir.glob("verify_log*.txt")))


def list_run_dirs(logs_root: pathlib.Path) -> list[pathlib.Path]:
    if not logs_root.exists():
        raise FileNotFoundError(f"logs root not found: {logs_root}")
    runs = [p for p in logs_root.iterdir() if p.is_dir() and _has_verify_log(p)]
    return sorted(runs, key=lambda p: p.name)


def resolve_runs(logs_root: pathlib.Path, runs: list[str] | None, latest: int | None) -> list[pathlib.Path]:
    all_runs = list_run_dirs(logs_root)
    by_name = {p.name: p for p in all_runs}

    if runs:
        out: list[pathlib.Path] = []
        for r in runs:
            p = pathlib.Path(r)
            if p.is_absolute():
                out.append(p)
            elif (logs_root / r).exists():
                out.append(logs_root / r)
            elif r in by_name:
                out.append(by_name[r])
            else:
                raise FileNotFoundError(f"run not found: {r}")
        return out

    if latest is not None:
        if latest <= 0:
            raise ValueError("--latest must be > 0")
        return all_runs[-latest:]

    if not all_runs:
        raise FileNotFoundError(f"no run dirs with verify logs under: {logs_root}")
    return [all_runs[-1]]


def pct(n: int, total: int) -> float:
    return (100.0 * n / total) if total else 0.0


def top_n_share(counter: collections.Counter[int], n: int) -> float:
    total = sum(counter.values())
    if total <= 0:
        return 0.0
    return sum(v for _, v in counter.most_common(n)) / float(total)


def calc_correct_count(cycles: list[dict[str, Any]], fixed: int | None, cutoff_time: str | None) -> int | None:
    if fixed is not None:
        return fixed
    if cutoff_time is None:
        return None
    for i, c in enumerate(cycles):
        if c["ts"] == cutoff_time:
            return i + 1
    return None


def summarize_run(run_dir: pathlib.Path, correct_count: int | None, cutoff_time: str | None) -> dict[str, Any]:
    lines = read_verify_lines(run_dir)
    cycles = extract_cycles(lines)
    if not cycles:
        raise RuntimeError(f"no verify decisions parsed: {run_dir}")

    gate = extract_gate_info(lines)
    total = len(cycles)
    decisions = collections.Counter(c["decision"] for c in cycles)

    gallery_all = collections.Counter(c["sigfm_gal"] for c in cycles if c["sigfm_gal"] >= 0)
    gallery_match = collections.Counter(
        c["sigfm_gal"] for c in cycles
        if c["decision"] == "MATCH" and c["sigfm_gal"] >= 0
    )

    binary_like = sum(
        1 for c in cycles
        if abs(c["best_sigfm"] - 0.0) <= 1e-9 or abs(c["best_sigfm"] - 1.0) <= 1e-9
    )

    ridges = gate.get("ridge_valid_gallery", [])
    vg_mean = (sum(ridges) / len(ridges)) if ridges else 0.0

    top1m = top_n_share(gallery_match, 1) * 100.0
    top2m = top_n_share(gallery_match, 2) * 100.0
    high_conc = (top1m >= 70.0) or (top2m >= 90.0 and len(gallery_match) <= 3)

    row: dict[str, Any] = {
        "run": run_dir.name,
        "cycles": total,
        "match": decisions.get("MATCH", 0),
        "no_match": decisions.get("NO_MATCH", 0),
        "retry": decisions.get("RETRY", 0),
        "match_pct": pct(decisions.get("MATCH", 0), total),
        "no_match_pct": pct(decisions.get("NO_MATCH", 0), total),
        "retry_pct": pct(decisions.get("RETRY", 0), total),
        "binary_pct": pct(binary_like, total),
        "valid_gallery_mean": vg_mean,
        "g_all": len(gallery_all),
        "g_match": len(gallery_match),
        "top1m": top1m,
        "top2m": top2m,
        "high_conc": high_conc,
    }

    cc = calc_correct_count(cycles, correct_count, cutoff_time)
    if cc is not None and 0 < cc < total:
        correct = cycles[:cc]
        wrong = cycles[cc:]

        c_match = sum(1 for c in correct if c["decision"] == "MATCH")
        w_match = sum(1 for c in wrong if c["decision"] == "MATCH")

        c_gallery = collections.Counter(
            c["sigfm_gal"] for c in correct
            if c["decision"] == "MATCH" and c["sigfm_gal"] >= 0
        )
        w_gallery = collections.Counter(
            c["sigfm_gal"] for c in wrong
            if c["decision"] == "MATCH" and c["sigfm_gal"] >= 0
        )

        row.update({
            "correct_count": cc,
            "tp_pct": pct(c_match, len(correct)),
            "fp_pct": pct(w_match, len(wrong)),
            "correct_top2": top_n_share(c_gallery, 2) * 100.0,
            "wrong_top2": top_n_share(w_gallery, 2) * 100.0,
        })

    return row


def print_table(rows: list[dict[str, Any]], split_enabled: bool) -> None:
    if split_enabled:
        header = (
            "run                cyc  m%    nm%   r%    bin%  vg   gA gM  top1m top2m  "
            "TP%   FP%   cTop2 wTop2 flag"
        )
        print(header)
        print("-" * len(header))
        for r in rows:
            tp = f"{r['tp_pct']:.1f}" if "tp_pct" in r else "n/a"
            fp = f"{r['fp_pct']:.1f}" if "fp_pct" in r else "n/a"
            ctop2 = f"{r['correct_top2']:.1f}" if "correct_top2" in r else "n/a"
            wtop2 = f"{r['wrong_top2']:.1f}" if "wrong_top2" in r else "n/a"
            print(
                f"{r['run']:<18} {r['cycles']:>3} "
                f"{r['match_pct']:>5.1f} {r['no_match_pct']:>5.1f} {r['retry_pct']:>5.1f} "
                f"{r['binary_pct']:>5.1f} {r['valid_gallery_mean']:>4.1f} "
                f"{r['g_all']:>2} {r['g_match']:>2} {r['top1m']:>6.1f} {r['top2m']:>6.1f} "
                f"{tp:>5} {fp:>5} "
                f"{ctop2:>6} {wtop2:>6} "
                f"{'HIGH' if r['high_conc'] else '-'}"
            )
    else:
        header = "run                cyc  m%    nm%   r%    bin%  vg   gA gM  top1m top2m flag"
        print(header)
        print("-" * len(header))
        for r in rows:
            print(
                f"{r['run']:<18} {r['cycles']:>3} "
                f"{r['match_pct']:>5.1f} {r['no_match_pct']:>5.1f} {r['retry_pct']:>5.1f} "
                f"{r['binary_pct']:>5.1f} {r['valid_gallery_mean']:>4.1f} "
                f"{r['g_all']:>2} {r['g_match']:>2} {r['top1m']:>6.1f} {r['top2m']:>6.1f} "
                f"{'HIGH' if r['high_conc'] else '-'}"
            )


def print_aggregate(rows: list[dict[str, Any]], split_enabled: bool) -> None:
    if not rows:
        return

    n = len(rows)
    avg_match = sum(r["match_pct"] for r in rows) / n
    split_rows = [r for r in rows if "tp_pct" in r and "fp_pct" in r]
    avg_fp = (sum(r["fp_pct"] for r in split_rows) / len(split_rows)) if split_rows else None
    avg_tp = (sum(r["tp_pct"] for r in split_rows) / len(split_rows)) if split_rows else None
    avg_top2m = sum(r["top2m"] for r in rows) / n
    high = sum(1 for r in rows if r["high_conc"])

    print("\nAggregate")
    print(f"- runs: {n}")
    print(f"- avg match%: {avg_match:.2f}")
    print(f"- avg top2 concentration (MATCH): {avg_top2m:.2f}%")
    print(f"- runs flagged high concentration: {high}/{n}")
    if split_enabled and split_rows:
        print(f"- avg TP% (split runs): {avg_tp:.2f}")
        print(f"- avg FP% (split runs): {avg_fp:.2f}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Batch verify summary with gallery concentration metrics")
    ap.add_argument("--logs-root", type=pathlib.Path, default=DEFAULT_LOGS_ROOT,
                    help=f"Run root directory (default: {DEFAULT_LOGS_ROOT})")
    ap.add_argument("--runs", nargs="*", default=None,
                    help="Specific run IDs or absolute run paths")
    ap.add_argument("--latest", type=int, default=None,
                    help="Use latest N runs from logs root")
    ap.add_argument("--correct-count", type=int, default=None,
                    help="Same correct-finger cycle count applied to all runs")
    ap.add_argument("--cutoff-time", default=None,
                    help="Same cutoff timestamp (HH:MM:SS.mmm) applied to all runs")
    ap.add_argument("--out", type=pathlib.Path, default=None,
                    help="Optional output text file")
    args = ap.parse_args()

    try:
        run_dirs = resolve_runs(args.logs_root, args.runs, args.latest)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    rows: list[dict[str, Any]] = []
    for run_dir in run_dirs:
        try:
            rows.append(summarize_run(run_dir, args.correct_count, args.cutoff_time))
        except Exception as exc:
            print(f"WARN: skipping {run_dir.name}: {exc}", file=sys.stderr)

    if not rows:
        print("ERROR: no runs summarized", file=sys.stderr)
        return 1

    split_enabled = any("tp_pct" in r for r in rows)

    output_lines: list[str] = []
    # Capture printed output while keeping implementation simple.
    from io import StringIO
    import contextlib

    buf = StringIO()
    with contextlib.redirect_stdout(buf):
        print(f"CB2000 VERIFY BATCH SUMMARY")
        print(f"logs_root: {args.logs_root}")
        print(f"runs: {', '.join(r['run'] for r in rows)}")
        print("")
        print_table(rows, split_enabled)
        print_aggregate(rows, split_enabled)
    text = buf.getvalue().rstrip() + "\n"

    print(text, end="")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
        print(f"\nSaved: {args.out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
