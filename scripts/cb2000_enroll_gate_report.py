#!/usr/bin/env python3
"""Summarize CB2000 enroll gate behavior from an enroll log."""

from __future__ import annotations

import argparse
import math
import re
from collections import Counter
from pathlib import Path
from statistics import median

from cb2000_paths import PROJECT_ROOT, default_enroll_log_input


TS_RE = re.compile(r"\b(\d{2}:\d{2}:\d{2}\.\d{3})\b")
PROGRESS_RE = re.compile(r"reported (\d+) of (\d+) have been completed")
ENROLL_QUALITY_RE = re.compile(
    r"\[ ENROLL_QUALITY \].*?count=(\d+) spread=([0-9.]+) peak=([0-9.]+) "
    r"min_count=(\d+) min_spread=([0-9.]+) min_peak=([0-9.]+)"
)
RETRY_RE = re.compile(
    r"\[ RETRY \] action=enroll cause=([a-zA-Z0-9_]+) total=(\d+) cause_count=(\d+)"
)
QUALITY_REJECT_RE = re.compile(
    r"Reject! enroll quality gate\. count=(\d+) spread=([0-9.]+) peak=([0-9.]+) "
    r"\(fail_count=(\d+) fail_spread=(\d+) fail_peak=(\d+)\)"
)
DIVERSITY_TELEMETRY_RE = re.compile(
    r"\[ ENROLL_DIVERSITY \] stage=(\d+).*?overlap=(\d+) best=([0-9.]+) mean=([0-9.]+) "
    r"pairs=(\d+).*?ovl=(\d+)\.\.(\d+) link=([0-9.]+)\.\.([0-9.]+).*?"
    r"success=(\d+)/(\d+) \(([0-9.]+) >= ([0-9.]+), min_pairs=(\d+)\)"
)
DIVERSITY_REJECT_RE = re.compile(
    r"Reject! enroll diversity gate\. overlap=(\d+) best=([0-9.]+) mean=([0-9.]+) "
    r"success=(\d+)/(\d+) ratio=([0-9.]+) threshold=([0-9.]+) "
    r"\(fail_ovl_low=(\d+) fail_ovl_high=(\d+) fail_link_low=(\d+) "
    r"fail_link_high=(\d+) fail_success=(\d+)\)"
)
LEGACY_GATE_RE = re.compile(
    r"\[ GATE \] action=enroll min_area=(\d+) fail_var=(\d+) fail_area=(\d+) fail_focus=(\d+)"
)
EFFECTIVE_GATE_RE = re.compile(
    r"\[ GATE \] action=enroll min_var=([0-9.]+) min_area=(\d+) min_focus=([0-9.]+) "
    r"fail_var=(\d+) fail_area=(\d+) fail_focus=(\d+)"
)
STAGE_FAIL_RE = re.compile(r"Enroll stage (\d+) of (\d+) failed with error (.+)\.")
STATS_RE = re.compile(
    r"\[ STATS \] accepted=(\d+) retry_total=(\d+).*? area=(\d+) quality=(\d+).*? enroll_div=(\d+)"
)
COPY_LINE_RE = re.compile(r"'([^']+)'\s*->\s*'([^']+)'")

MACRO_PATTERNS = {
    "quality_min_count": r"#define\s+CB2000_ENROLL_MIN_RIDGE_COUNT_DEFAULT\s+([0-9.]+)",
    "quality_min_spread": r"#define\s+CB2000_ENROLL_MIN_RIDGE_SPREAD_DEFAULT\s+([0-9.]+)",
    "quality_min_peak": r"#define\s+CB2000_ENROLL_MIN_RIDGE_PEAK_DEFAULT\s+([0-9.]+)",
    "div_overlap_min": r"#define\s+CB2000_ENROLL_DIVERSITY_MIN_OVERLAP_DEFAULT\s+([0-9.]+)",
    "div_overlap_max": r"#define\s+CB2000_ENROLL_DIVERSITY_MAX_OVERLAP_DEFAULT\s+([0-9.]+)",
    "div_link_min": r"#define\s+CB2000_ENROLL_DIVERSITY_LINK_MIN_DEFAULT\s+([0-9.]+)",
    "div_link_max": r"#define\s+CB2000_ENROLL_DIVERSITY_LINK_MAX_DEFAULT\s+([0-9.]+)",
    "div_success_ratio": r"#define\s+CB2000_ENROLL_SUCCESS_RATIO_MIN_DEFAULT\s+([0-9.]+)",
    "div_success_min_pairs": r"#define\s+CB2000_ENROLL_SUCCESS_MIN_PAIRS_DEFAULT\s+([0-9.]+)",
}


def parse_ts(line: str) -> str | None:
    match = TS_RE.search(line)
    return match.group(1) if match else None


def quantile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    idx = (len(ordered) - 1) * q
    lo = math.floor(idx)
    hi = math.ceil(idx)
    if lo == hi:
        return ordered[lo]
    frac = idx - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def fmt_stats(values: list[float], precision: int = 3) -> str:
    if not values:
        return "n/a"
    return (
        f"min={min(values):.{precision}f} "
        f"p50={median(values):.{precision}f} "
        f"p90={quantile(values, 0.90):.{precision}f} "
        f"max={max(values):.{precision}f}"
    )


def load_defaults_from_source(source_path: Path) -> dict[str, float]:
    defaults: dict[str, float] = {}
    if not source_path.exists():
        return defaults
    text = source_path.read_text(encoding="utf-8", errors="ignore")
    for key, pattern in MACRO_PATTERNS.items():
        match = re.search(pattern, text)
        if match:
            defaults[key] = float(match.group(1))
    return defaults


def load_defaults(root: Path) -> dict[str, float]:
    src = root / "src" / "canvasbio_cb2000.c"
    return load_defaults_from_source(src)


def resolve_run_driver_source(run_dir: Path) -> Path | None:
    copy_log = run_dir / "copy_driver.log"
    if not copy_log.exists():
        return None

    with copy_log.open("r", encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            match = COPY_LINE_RE.search(line)
            if not match:
                continue
            src = Path(match.group(1))
            dst = match.group(2)
            if dst.endswith("/canvasbio_cb2000.c"):
                return src

    return None


def format_sorted_counter(counter: Counter, prefix: str = "- ") -> list[str]:
    if not counter:
        return [f"{prefix}(none)"]
    lines = []
    for key, value in sorted(counter.items(), key=lambda item: (-item[1], str(item[0]))):
        lines.append(f"{prefix}{key}: {value}")
    return lines


def resolve_input_path(input_arg: str) -> Path:
    path = Path(input_arg)
    if path.is_dir():
        for name in ("enroll_log.txt", "enroll.log"):
            candidate = path / name
            if candidate.exists():
                return candidate
        return path / "enroll_log.txt"
    return path


def main() -> int:
    default_input = default_enroll_log_input()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input",
        nargs="?",
        default=None,
        help=f"Path to enroll log or run directory (default: {default_input})",
    )
    parser.add_argument(
        "--out",
        default=None,
        help="Optional output file path. Default: <run_dir>/enroll_gate_report.txt",
    )
    args = parser.parse_args()

    input_arg = args.input if args.input is not None else str(default_input)
    log_path = resolve_input_path(input_arg).expanduser().resolve()
    if not log_path.exists():
        raise SystemExit(f"ERROR: log not found: {log_path}")

    run_dir = log_path.parent
    defaults = load_defaults(PROJECT_ROOT)

    progress_points: list[tuple[str | None, int, int]] = []
    retry_causes: Counter = Counter()
    retry_total_max = 0
    quality_rejects: list[dict[str, float]] = []
    diversity_telemetry: list[dict[str, float]] = []
    diversity_rejects: list[dict[str, float]] = []
    quality_runtime_thresholds: list[tuple[float, float, float]] = []
    legacy_gate_fail = Counter()
    stage_error_counts: Counter = Counter()
    stage_fail_counts: Counter = Counter()
    stale_count = 0
    attempts = 0
    first_ts = None
    last_ts = None
    final_stats = None
    final_progress = None

    with log_path.open("r", encoding="utf-8", errors="ignore") as handle:
        for raw_line in handle:
            line = raw_line.rstrip("\n")
            ts = parse_ts(line)
            if ts is not None:
                if first_ts is None:
                    first_ts = ts
                last_ts = ts

            if "=== ENROLL attempt" in line:
                attempts += 1

            progress_match = PROGRESS_RE.search(line)
            if progress_match:
                completed = int(progress_match.group(1))
                total = int(progress_match.group(2))
                progress_points.append((ts, completed, total))
                if final_progress is None or completed >= final_progress[0]:
                    final_progress = (completed, total)

            quality_runtime_match = ENROLL_QUALITY_RE.search(line)
            if quality_runtime_match:
                quality_runtime_thresholds.append(
                    (
                        float(quality_runtime_match.group(4)),
                        float(quality_runtime_match.group(5)),
                        float(quality_runtime_match.group(6)),
                    )
                )

            retry_match = RETRY_RE.search(line)
            if retry_match:
                cause = retry_match.group(1)
                total = int(retry_match.group(2))
                retry_causes[cause] += 1
                retry_total_max = max(retry_total_max, total)

            quality_match = QUALITY_REJECT_RE.search(line)
            if quality_match:
                quality_rejects.append(
                    {
                        "count": float(quality_match.group(1)),
                        "spread": float(quality_match.group(2)),
                        "peak": float(quality_match.group(3)),
                        "fail_count": float(quality_match.group(4)),
                        "fail_spread": float(quality_match.group(5)),
                        "fail_peak": float(quality_match.group(6)),
                    }
                )

            diversity_match = DIVERSITY_TELEMETRY_RE.search(line)
            if diversity_match:
                diversity_telemetry.append(
                    {
                        "stage": float(diversity_match.group(1)),
                        "overlap": float(diversity_match.group(2)),
                        "best": float(diversity_match.group(3)),
                        "mean": float(diversity_match.group(4)),
                        "pairs": float(diversity_match.group(5)),
                        "ovl_min": float(diversity_match.group(6)),
                        "ovl_max": float(diversity_match.group(7)),
                        "link_min": float(diversity_match.group(8)),
                        "link_max": float(diversity_match.group(9)),
                        "success_num": float(diversity_match.group(10)),
                        "success_den": float(diversity_match.group(11)),
                        "success_ratio": float(diversity_match.group(12)),
                        "success_threshold": float(diversity_match.group(13)),
                        "min_pairs": float(diversity_match.group(14)),
                    }
                )

            diversity_reject_match = DIVERSITY_REJECT_RE.search(line)
            if diversity_reject_match:
                diversity_rejects.append(
                    {
                        "overlap": float(diversity_reject_match.group(1)),
                        "best": float(diversity_reject_match.group(2)),
                        "mean": float(diversity_reject_match.group(3)),
                        "success_num": float(diversity_reject_match.group(4)),
                        "success_den": float(diversity_reject_match.group(5)),
                        "ratio": float(diversity_reject_match.group(6)),
                        "threshold": float(diversity_reject_match.group(7)),
                        "fail_ovl_low": float(diversity_reject_match.group(8)),
                        "fail_ovl_high": float(diversity_reject_match.group(9)),
                        "fail_link_low": float(diversity_reject_match.group(10)),
                        "fail_link_high": float(diversity_reject_match.group(11)),
                        "fail_success": float(diversity_reject_match.group(12)),
                    }
                )

            gate_match = EFFECTIVE_GATE_RE.search(line)
            if gate_match:
                fail_var = int(gate_match.group(4))
                fail_area = int(gate_match.group(5))
                fail_focus = int(gate_match.group(6))
                if fail_var:
                    legacy_gate_fail["fail_var"] += 1
                if fail_area:
                    legacy_gate_fail["fail_area"] += 1
                if fail_focus:
                    legacy_gate_fail["fail_focus"] += 1

            legacy_match = LEGACY_GATE_RE.search(line)
            if legacy_match:
                fail_var = int(legacy_match.group(2))
                fail_area = int(legacy_match.group(3))
                fail_focus = int(legacy_match.group(4))
                if fail_var:
                    legacy_gate_fail["fail_var"] += 1
                if fail_area:
                    legacy_gate_fail["fail_area"] += 1
                if fail_focus:
                    legacy_gate_fail["fail_focus"] += 1

            stage_fail_match = STAGE_FAIL_RE.search(line)
            if stage_fail_match:
                stage = int(stage_fail_match.group(1))
                message = stage_fail_match.group(3)
                stage_fail_counts[stage] += 1
                stage_error_counts[message] += 1

            if "Finger polling stale" in line:
                stale_count += 1

            stats_match = STATS_RE.search(line)
            if stats_match:
                final_stats = {
                    "accepted": int(stats_match.group(1)),
                    "retry_total": int(stats_match.group(2)),
                    "area": int(stats_match.group(3)),
                    "quality": int(stats_match.group(4)),
                    "enroll_div": int(stats_match.group(5)),
                }

    accepted = 0
    target = 15
    accepted_candidates: list[int] = []
    if final_progress is not None:
        accepted_candidates.append(final_progress[0])
        target = final_progress[1]
    if final_stats is not None:
        accepted_candidates.append(final_stats["accepted"])
    if accepted_candidates:
        accepted = max(accepted_candidates)

    quality_fail_count = int(sum(item["fail_count"] for item in quality_rejects))
    quality_fail_spread = int(sum(item["fail_spread"] for item in quality_rejects))
    quality_fail_peak = int(sum(item["fail_peak"] for item in quality_rejects))

    diversity_fail_counters = Counter()
    for item in diversity_rejects:
        for key in ("fail_ovl_low", "fail_ovl_high", "fail_link_low", "fail_link_high", "fail_success"):
            if item[key] > 0:
                diversity_fail_counters[key] += 1

    lines: list[str] = []
    lines.append("CB2000 ENROLL GATE REPORT")
    lines.append("=" * 80)
    lines.append(f"Log file: {log_path}")
    lines.append(f"Run dir : {run_dir}")
    lines.append(f"Attempts: {attempts}")
    lines.append(f"Time span: {first_ts or 'n/a'} -> {last_ts or 'n/a'}")
    lines.append("")
    lines.append("[SUMMARY]")
    lines.append(f"Accepted stages: {accepted}/{target}")
    lines.append(f"Total retries : {retry_total_max}")
    lines.append("Retry causes:")
    lines.extend(format_sorted_counter(retry_causes))
    lines.append("")

    if progress_points:
        stage_first_seen: dict[int, str] = {}
        for ts, completed, _ in progress_points:
            if completed not in stage_first_seen and ts:
                stage_first_seen[completed] = ts
        lines.append("[STAGE PROGRESSION]")
        for stage in sorted(stage_first_seen):
            lines.append(f"- stage {stage}: first seen at {stage_first_seen[stage]}")
        lines.append("")

    lines.append("[QUALITY GATE]")
    if not quality_rejects:
        lines.append("- No quality gate rejections found.")
    else:
        counts = [item["count"] for item in quality_rejects]
        spreads = [item["spread"] for item in quality_rejects]
        peaks = [item["peak"] for item in quality_rejects]
        lines.append(f"- quality rejects: {len(quality_rejects)}")
        lines.append(
            f"- fail flags: count={quality_fail_count} spread={quality_fail_spread} peak={quality_fail_peak}"
        )
        lines.append(f"- count stats : {fmt_stats(counts, precision=1)}")
        lines.append(f"- spread stats: {fmt_stats(spreads, precision=3)}")
        lines.append(f"- peak stats  : {fmt_stats(peaks, precision=1)}")

        default_count = defaults.get("quality_min_count", 78.0)
        default_spread = defaults.get("quality_min_spread", 0.92)
        default_peak = defaults.get("quality_min_peak", 470.0)
        threshold_source = "project defaults (src/canvasbio_cb2000.c)"
        run_driver_source = resolve_run_driver_source(run_dir)
        run_defaults: dict[str, float] = {}

        if run_driver_source is not None:
            run_defaults = load_defaults_from_source(run_driver_source)

        if quality_runtime_thresholds:
            last_runtime_thresholds = quality_runtime_thresholds[-1]
            default_count, default_spread, default_peak = last_runtime_thresholds
            threshold_source = "runtime ENROLL_QUALITY telemetry"
        elif run_defaults:
            default_count = run_defaults.get("quality_min_count", default_count)
            default_spread = run_defaults.get("quality_min_spread", default_spread)
            default_peak = run_defaults.get("quality_min_peak", default_peak)
            threshold_source = f"run source ({run_driver_source})"

        lines.append(f"- threshold source: {threshold_source}")
        lines.append(
            f"- current thresholds: count>={default_count:.0f} spread>={default_spread:.3f} peak>={default_peak:.1f}"
        )

        sim_thresholds = [
            ("current", default_count, default_spread, default_peak),
            ("peak>=460", default_count, default_spread, 460.0),
            ("spread>=0.900", default_count, 0.900, default_peak),
            ("spread>=0.900 + peak>=460", default_count, 0.900, 460.0),
            ("spread>=0.900 + peak>=455", default_count, 0.900, 455.0),
            ("count>=76 + spread>=0.900 + peak>=460", 76.0, 0.900, 460.0),
        ]
        lines.append("- simulation over current quality-reject set:")
        for label, thr_count, thr_spread, thr_peak in sim_thresholds:
            would_pass = sum(
                1
                for item in quality_rejects
                if item["count"] >= thr_count
                and item["spread"] >= thr_spread
                and item["peak"] >= thr_peak
            )
            ratio = (100.0 * would_pass / len(quality_rejects)) if quality_rejects else 0.0
            lines.append(f"  - {label}: {would_pass}/{len(quality_rejects)} ({ratio:.1f}%) would pass")
    lines.append("")

    lines.append("[DIVERSITY GATE]")
    if not diversity_rejects:
        lines.append("- No diversity gate rejections found.")
    else:
        overlaps = [item["overlap"] for item in diversity_rejects]
        best_scores = [item["best"] for item in diversity_rejects]
        lines.append(f"- diversity rejects: {len(diversity_rejects)}")
        lines.append("- fail flags:")
        lines.extend(format_sorted_counter(diversity_fail_counters, prefix="  - "))
        lines.append(f"- overlap stats: {fmt_stats(overlaps, precision=1)}")
        lines.append(f"- best-link stats: {fmt_stats(best_scores, precision=3)}")

        only_high_overlap = [
            item
            for item in diversity_rejects
            if item["fail_ovl_high"] > 0
            and item["fail_ovl_low"] == 0
            and item["fail_link_low"] == 0
            and item["fail_link_high"] == 0
            and item["fail_success"] == 0
        ]
        lines.append(f"- only high-overlap blockers: {len(only_high_overlap)}/{len(diversity_rejects)}")
        for candidate in (96, 97, 98, 99):
            rescued = sum(1 for item in only_high_overlap if item["overlap"] <= candidate)
            ratio = (100.0 * rescued / len(diversity_rejects)) if diversity_rejects else 0.0
            lines.append(
                f"  - if max_overlap={candidate}: recovers {rescued}/{len(diversity_rejects)} ({ratio:.1f}%)"
            )
    lines.append("")

    lines.append("[LEGACY GATE / OTHER]")
    lines.append(f"- legacy gate hits: {sum(legacy_gate_fail.values())}")
    lines.extend(format_sorted_counter(legacy_gate_fail))
    lines.append(f"- 'Finger polling stale' events: {stale_count}")
    lines.append("")

    lines.append("[STAGE FAILURES]")
    lines.append("By stage:")
    lines.extend(format_sorted_counter(stage_fail_counts))
    lines.append("By error text:")
    lines.extend(format_sorted_counter(stage_error_counts))
    lines.append("")

    lines.append("[TUNING NOTES]")
    if quality_rejects:
        peak_p90 = quantile([item["peak"] for item in quality_rejects], 0.9)
        lines.append(
            f"- Most quality rejects are close to peak threshold; p90 peak in rejects is {peak_p90:.1f}."
        )
    if diversity_rejects and diversity_fail_counters.get("fail_ovl_high", 0) > 0:
        lines.append("- Diversity rejects are mostly high-overlap; operator guidance should force micro-movements.")
    lines.append("- Keep FP safety first: relax in small steps and retest wrong-finger leakage after each change.")

    report_text = "\n".join(lines) + "\n"
    out_path = Path(args.out).expanduser().resolve() if args.out else run_dir / "enroll_gate_report.txt"
    out_path.write_text(report_text, encoding="utf-8")
    print(report_text)
    print(f"Report saved to: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
