#!/usr/bin/env python3
"""
Generate a comprehensive verify report for a CB2000 test run.

Reads verify_log.txt from a run directory and produces:
- Per-cycle telemetry table (SIGFM scores + optional diversity NCC telemetry)
- Correct vs wrong finger split (using --correct-count or --cutoff-time)
- Acceptance criteria check (FP=0%, TP>=75%)
- Failure analysis
- Saves report to logs/<run_id>/report.txt

Default run root:
  $CB2000_RUNTIME_ROOT/runs when available, otherwise $CB2000_LOG_ROOT or
  ../dev_logs/sessions

Usage:
  cb2000_run_report.py                                  # latest run, no split
  cb2000_run_report.py --correct-count 16               # first 16 = correct finger
  cb2000_run_report.py --cutoff-time 11:11:36.664       # correct finger up to this time
  cb2000_run_report.py --run 20260225_111015 --correct-count 16
  cb2000_run_report.py --run /absolute/path/to/run_dir --correct-count 16
"""

from __future__ import annotations

import argparse
import collections
import os
import pathlib
import re
import sys
import textwrap
from datetime import datetime

from cb2000_paths import DEFAULT_RUNS_ROOT

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

DEFAULT_LOGS_ROOT = DEFAULT_RUNS_ROOT
DEFAULT_REPORT_DRIVER_TAG = os.environ.get("CB2000_REPORT_DRIVER_TAG", "B1")

# ---------------------------------------------------------------------------
# Regexes
# ---------------------------------------------------------------------------

RE_SIGFM = re.compile(
    r"(\d{2}:\d{2}:\d{2}\.\d{3}).*\[\s*SIGFM\s*\]\s+"
    r"gallery=(\d+)\s+score=(\S+).*?consensus=(\d+).*?inlier_ratio=(\S+)"
)
RE_NCC = re.compile(
    r"(\d{2}:\d{2}:\d{2}\.\d{3}).*\[\s*NCC\s*\]\s+"
    r"gallery=(\d+)\s+int=(\S+)\s+grad=(\S+)"
)
RE_DECISION = re.compile(
    r"(\d{2}:\d{2}:\d{2}\.\d{3}).*\[\s*(?:SIGFM_MATCH|SIGFM_NCC|NCC)\s*\]\s+"
    r"(?:action=\S+\s+)?decision=(\S+)\s*->\s*result=(.+)$"
)
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
RE_MATRIX = re.compile(r"\[ VERIFY_READY_MATRIX \].* => (?P<matrix>[A-Z_]+)")
RE_THRESHOLDS = re.compile(
    r"\[\s*(?:SIGFM_MATCH|SIGFM_NCC)\s*\]\s+thresholds:\s+(.+)"
)
RE_RIDGE_PROBE = re.compile(
    r"\[\s*RIDGE_TELEMETRY\s*\]\s+probe\b.*?\bvalid_gallery=(\d+)"
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _has_verify_log(run_dir: pathlib.Path) -> bool:
    """Check if a run directory has any verify log file."""
    return ((run_dir / "verify_log.txt").exists() or
            (run_dir / "verify.log").exists())


def pick_latest_run(logs_root: pathlib.Path) -> pathlib.Path:
    if not logs_root.exists():
        raise FileNotFoundError(f"logs root not found: {logs_root}")
    dirs = sorted(
        [p for p in logs_root.iterdir() if p.is_dir() and _has_verify_log(p)],
        key=lambda p: p.name,
    )
    if not dirs:
        raise FileNotFoundError(f"no run dirs with verify log under: {logs_root}")
    return dirs[-1]


def resolve_run_dir(run_arg: str | None, logs_root: pathlib.Path) -> pathlib.Path:
    if run_arg is None:
        return pick_latest_run(logs_root)
    p = pathlib.Path(run_arg)
    if p.is_absolute():
        return p
    # Treat as run ID under logs_root
    candidate = logs_root / run_arg
    if candidate.exists():
        return candidate
    return p


def read_verify_lines(run_dir: pathlib.Path) -> list[str]:
    # Try canonical name first, then alternate
    for name in ("verify_log.txt", "verify.log"):
        verify_log = run_dir / name
        if verify_log.exists():
            return verify_log.read_text(encoding="utf-8", errors="replace").splitlines()
    # Fallback: individual attempt logs
    logs = sorted(run_dir.glob("verify_log*.txt"))
    lines: list[str] = []
    for lp in logs:
        try:
            lines.extend(lp.read_text(encoding="utf-8", errors="replace").splitlines())
        except OSError:
            continue
    return lines


# ---------------------------------------------------------------------------
# Cycle extraction
# ---------------------------------------------------------------------------


def extract_cycles(lines: list[str]) -> list[dict]:
    cycles = []
    cur_sigfm: dict[int, tuple[float, int, float]] = {}
    cur_ncc: dict[int, tuple[float, float]] = {}

    for line in lines:
        m = RE_NCC.search(line)
        if m:
            _, gal, intensity, grad = m.groups()
            cur_ncc[int(gal)] = (float(intensity), float(grad))
            continue

        m = RE_SIGFM.search(line)
        if m:
            _, gal, score, cons, inlier = m.groups()
            cur_sigfm[int(gal)] = (float(score), int(cons), float(inlier))
            continue

        m = RE_DECISION.search(line)
        if m:
            ts, decision, result = m.groups()

            # Best SIGFM gallery
            if cur_sigfm:
                best_gal = max(cur_sigfm, key=lambda g: cur_sigfm[g][0])
                best_sigfm, best_cons, best_inlier = cur_sigfm[best_gal]
                sigfm_sup = sum(1 for g in cur_sigfm if cur_sigfm[g][0] >= 0.10)
            else:
                best_gal, best_sigfm, best_cons, best_inlier, sigfm_sup = -1, 0.0, 0, 0.0, 0

            # Best NCC gallery
            if cur_ncc:
                ncc_best_gal = max(cur_ncc, key=lambda g: cur_ncc[g][0])
                best_int, best_grad = cur_ncc[ncc_best_gal]
                ncc_sup = sum(1 for g in cur_ncc if cur_ncc[g][0] >= 0.10)
            else:
                ncc_best_gal, best_int, best_grad, ncc_sup = -1, 0.0, 0.0, 0

            # All per-gallery scores for detail section
            all_sigfm = dict(cur_sigfm)
            all_ncc = dict(cur_ncc)

            cycles.append({
                "ts": ts,
                "decision": decision,
                "result": result.strip(),
                "best_sigfm": best_sigfm,
                "sigfm_gal": best_gal,
                "cons": best_cons,
                "inlier": best_inlier,
                "sigfm_sup": sigfm_sup,
                "best_int": best_int,
                "ncc_gal": ncc_best_gal,
                "best_grad": best_grad,
                "ncc_sup": ncc_sup,
                "all_sigfm": all_sigfm,
                "all_ncc": all_ncc,
            })
            cur_sigfm = {}
            cur_ncc = {}

    return cycles


def extract_gate_info(lines: list[str]) -> dict:
    gate_decisions = collections.Counter()
    gate_classes = collections.Counter()
    matrix_counts = collections.Counter()
    thresholds_line = ""
    ridge_valid_gallery: list[int] = []

    for line in lines:
        m = RE_GATE.search(line)
        if m:
            gate_decisions[m.group("decision")] += 1
            gate_classes[m.group("result_class")] += 1
            continue
        m = RE_MATRIX.search(line)
        if m:
            matrix_counts[m.group("matrix")] += 1
            continue
        m = RE_THRESHOLDS.search(line)
        if m and not thresholds_line:
            thresholds_line = m.group(1)
        m = RE_RIDGE_PROBE.search(line)
        if m:
            ridge_valid_gallery.append(int(m.group(1)))

    return {
        "gate_decisions": gate_decisions,
        "gate_classes": gate_classes,
        "matrix_counts": matrix_counts,
        "thresholds": thresholds_line,
        "ridge_valid_gallery": ridge_valid_gallery,
    }


# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------


def pct(n: int, total: int) -> float:
    return (100.0 * n / total) if total else 0.0


def top_n_share(counter: collections.Counter[int], n: int, total: int) -> float:
    if total <= 0 or not counter:
        return 0.0
    return sum(v for _, v in counter.most_common(n)) / float(total)


def emit_gallery_counter(
    out: list[str],
    title: str,
    counter: collections.Counter[int],
    total: int,
    max_rows: int = 8,
) -> None:
    out.append(title)
    if total <= 0 or not counter:
        out.append("  (none)")
        return

    for gal, cnt in counter.most_common(max_rows):
        out.append(f"  g{gal}: {cnt:>3} ({pct(cnt, total):.1f}%)")


def format_report(
    run_dir: pathlib.Path,
    cycles: list[dict],
    gate_info: dict,
    correct_count: int | None,
    driver_tag: str,
) -> str:
    out: list[str] = []
    w = out.append

    run_id = run_dir.name
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    total = len(cycles)

    w(f"{'=' * 90}")
    w(f"CB2000 {driver_tag} VERIFY REPORT - Run {run_id}")
    w(f"Generated: {now}")
    w(f"Run dir: {run_dir}")
    w(f"Total verify cycles: {total}")
    if correct_count is not None:
        w(f"Correct finger: cycles 1-{correct_count} | Wrong finger: cycles {correct_count + 1}-{total}")
    w(f"{'=' * 90}")
    w("")

    # Thresholds
    if gate_info["thresholds"]:
        w("[CLASSIFIER THRESHOLDS]")
        w(f"  {gate_info['thresholds']}")
        w("")

    # Gate info
    if gate_info["matrix_counts"]:
        w("[VERIFY ROUTING]")
        for key, val in gate_info["matrix_counts"].most_common():
            w(f"  {val:4d}  {key}")
        w("")

    # Per-cycle table
    w("[PER-CYCLE TELEMETRY]")
    hdr = f"{'#':>2} {'Time':>12} {'Decision':>10} | {'SIGFM':>6} {'g':>1} {'cons':>4} {'inlier':>6} {'s_sup':>5} | {'NCC_i':>6} {'g':>1} {'grad':>6} {'n_sup':>5}"
    w(hdr)
    w("-" * len(hdr))

    for i, c in enumerate(cycles, 1):
        marker = ""
        if correct_count is not None:
            if i == correct_count:
                marker = " <<< last correct"
            elif i == correct_count + 1:
                marker = " <<< wrong finger"
        w(
            f"{i:>2} {c['ts']:>12} {c['decision']:>10} | "
            f"{c['best_sigfm']:>6.4f} {c['sigfm_gal']:>1} {c['cons']:>4} {c['inlier']:>6.3f} {c['sigfm_sup']:>5} | "
            f"{c['best_int']:>6.4f} {c['ncc_gal']:>1} {c['best_grad']:>6.4f} {c['ncc_sup']:>5}{marker}"
        )
    w("")

    # Overall counts
    all_decisions = [c["decision"] for c in cycles]
    counts = collections.Counter(all_decisions)
    w("[ALL ATTEMPTS]")
    w(f"  total={total}")
    w(f"  MATCH:    {counts.get('MATCH', 0):>3} ({pct(counts.get('MATCH', 0), total):.1f}%)")
    w(f"  NO_MATCH: {counts.get('NO_MATCH', 0):>3} ({pct(counts.get('NO_MATCH', 0), total):.1f}%)")
    w(f"  RETRY:    {counts.get('RETRY', 0):>3} ({pct(counts.get('RETRY', 0), total):.1f}%)")
    w("")

    # Peak gallery concentration analysis
    best_gallery_all = collections.Counter(
        c["sigfm_gal"] for c in cycles if c["sigfm_gal"] >= 0
    )
    match_cycles = [c for c in cycles if c["decision"] == "MATCH" and c["sigfm_gal"] >= 0]
    best_gallery_match = collections.Counter(c["sigfm_gal"] for c in match_cycles)
    binary_like = sum(
        1
        for c in cycles
        if abs(c["best_sigfm"] - 0.0) <= 1e-9 or abs(c["best_sigfm"] - 1.0) <= 1e-9
    )

    w("[PEAK GALLERY ANALYSIS]")
    w(f"  cycles with gallery telemetry: {sum(best_gallery_all.values())}/{total}")
    w(f"  unique best galleries (all cycles): {len(best_gallery_all)}")
    w(f"  binary-like top scores (0.0/1.0): {binary_like}/{total} ({pct(binary_like, total):.1f}%)")
    if gate_info["ridge_valid_gallery"]:
        vg = gate_info["ridge_valid_gallery"]
        vg_mean = sum(vg) / len(vg)
        w(f"  RIDGE valid_gallery (probe): min={min(vg)} max={max(vg)} mean={vg_mean:.2f}")
    emit_gallery_counter(out, "  best gallery distribution (all cycles):", best_gallery_all, total)
    w(f"  MATCH cycles with best gallery: {sum(best_gallery_match.values())}")
    w(f"  unique best galleries among MATCH: {len(best_gallery_match)}")
    emit_gallery_counter(out, "  best gallery distribution (MATCH only):", best_gallery_match, sum(best_gallery_match.values()))
    if sum(best_gallery_match.values()) > 0:
        top1 = top_n_share(best_gallery_match, 1, sum(best_gallery_match.values()))
        top2 = top_n_share(best_gallery_match, 2, sum(best_gallery_match.values()))
        w(f"  concentration (MATCH): top1={top1*100:.1f}% top2={top2*100:.1f}%")
        if top1 >= 0.70 or (top2 >= 0.90 and len(best_gallery_match) <= 3):
            w("  FLAG: MATCH concentration is high (dominant gallery subset)")
    w("")

    # Split analysis
    if correct_count is not None and 0 < correct_count < total:
        correct = cycles[:correct_count]
        wrong = cycles[correct_count:]

        c_match = sum(1 for c in correct if c["decision"] == "MATCH")
        c_nomatch = sum(1 for c in correct if c["decision"] == "NO_MATCH")
        c_retry = sum(1 for c in correct if c["decision"] == "RETRY")
        c_total = len(correct)

        w_match = sum(1 for c in wrong if c["decision"] == "MATCH")
        w_nomatch = sum(1 for c in wrong if c["decision"] == "NO_MATCH")
        w_retry = sum(1 for c in wrong if c["decision"] == "RETRY")
        w_total = len(wrong)

        sigfm_c = [c["best_sigfm"] for c in correct]
        ncc_c = [c["best_int"] for c in correct]
        sigfm_w = [c["best_sigfm"] for c in wrong]
        ncc_w = [c["best_int"] for c in wrong]

        w(f"[CORRECT FINGER] ({c_total} attempts)")
        w(f"  MATCH:    {c_match:>3} ({pct(c_match, c_total):.1f}%)  <- True Positive rate")
        w(f"  NO_MATCH: {c_nomatch:>3} ({pct(c_nomatch, c_total):.1f}%)  <- False Negative")
        w(f"  RETRY:    {c_retry:>3} ({pct(c_retry, c_total):.1f}%)  <- Indeterminate")
        w(f"  SIGFM range: {min(sigfm_c):.4f} - {max(sigfm_c):.4f} (mean {sum(sigfm_c)/len(sigfm_c):.4f})")
        w(f"  NCC_i range: {min(ncc_c):.4f} - {max(ncc_c):.4f} (mean {sum(ncc_c)/len(ncc_c):.4f})")
        w("")

        w(f"[WRONG FINGER] ({w_total} attempts)")
        w(f"  MATCH:    {w_match:>3} ({pct(w_match, w_total):.1f}%)  <- False Positive rate")
        w(f"  NO_MATCH: {w_nomatch:>3} ({pct(w_nomatch, w_total):.1f}%)  <- True Negative")
        w(f"  RETRY:    {w_retry:>3} ({pct(w_retry, w_total):.1f}%)  <- Indeterminate")
        w(f"  SIGFM range: {min(sigfm_w):.4f} - {max(sigfm_w):.4f} (mean {sum(sigfm_w)/len(sigfm_w):.4f})")
        w(f"  NCC_i range: {min(ncc_w):.4f} - {max(ncc_w):.4f} (mean {sum(ncc_w)/len(ncc_w):.4f})")
        w("")

        c_match_gallery = collections.Counter(
            c["sigfm_gal"] for c in correct if c["decision"] == "MATCH" and c["sigfm_gal"] >= 0
        )
        w_match_gallery = collections.Counter(
            c["sigfm_gal"] for c in wrong if c["decision"] == "MATCH" and c["sigfm_gal"] >= 0
        )
        w("[PEAK GALLERY SPLIT]")
        w(f"  correct MATCH cycles with best gallery: {sum(c_match_gallery.values())}")
        emit_gallery_counter(out, "  correct best gallery (MATCH only):", c_match_gallery, sum(c_match_gallery.values()))
        if sum(c_match_gallery.values()) > 0:
            c_top2 = top_n_share(c_match_gallery, 2, sum(c_match_gallery.values()))
            w(f"  correct concentration (MATCH): top2={c_top2*100:.1f}%")
        w(f"  wrong-finger MATCH cycles with best gallery: {sum(w_match_gallery.values())}")
        emit_gallery_counter(out, "  wrong best gallery (MATCH only):", w_match_gallery, sum(w_match_gallery.values()))
        w("")

        # Acceptance criteria
        tp = pct(c_match, c_total)
        fp = pct(w_match, w_total)
        w("[ACCEPTANCE CRITERIA]")
        w(f"  FP = {fp:.1f}%    -> {'PASS' if w_match == 0 else 'FAIL'} (target: 0%)")
        w(f"  TP = {tp:.1f}%  -> {'PASS' if tp >= 75 else 'FAIL'} (target: >= 75%)")
        w("")

        # Failure analysis
        misses = [(i, c) for i, c in enumerate(correct, 1) if c["decision"] != "MATCH"]
        w("[FAILURE ANALYSIS] (correct finger misses)")
        if not misses:
            w("  (none - all correct finger attempts matched)")
        for idx, c in misses:
            w(
                f"  Cycle {idx} ({c['ts']}): {c['decision']}"
                f" | sigfm={c['best_sigfm']:.4f} g={c['sigfm_gal']}"
                f" cons={c['cons']} inlier={c['inlier']:.3f} sup={c['sigfm_sup']}"
                f" | ncc_i={c['best_int']:.4f} ncc_sup={c['ncc_sup']}"
            )
            # Per-gallery detail for failures
            if c["all_sigfm"]:
                for g in sorted(c["all_sigfm"]):
                    s_score, s_cons, s_inl = c["all_sigfm"][g]
                    n_int, n_grad = c["all_ncc"].get(g, (0.0, 0.0))
                    w(
                        f"    gallery={g}: sigfm={s_score:.4f} cons={s_cons}"
                        f" inlier={s_inl:.3f} | ncc_i={n_int:.4f} grad={n_grad:.4f}"
                    )
        w("")

        # Wrong finger leaks
        leaks = [(i + correct_count, c) for i, c in enumerate(wrong, 1) if c["decision"] == "RETRY"]
        w("[WRONG FINGER LEAKS] (RETRY = not rejected)")
        if not leaks:
            w("  (none - all wrong finger attempts rejected)")
        for idx, c in leaks:
            w(
                f"  Cycle {idx} ({c['ts']}): RETRY"
                f" | sigfm={c['best_sigfm']:.4f} g={c['sigfm_gal']}"
                f" cons={c['cons']} inlier={c['inlier']:.3f} sup={c['sigfm_sup']}"
                f" | ncc_i={c['best_int']:.4f} ncc_sup={c['ncc_sup']}"
            )
        w("")

    # Score distribution histogram (text)
    w("[SIGFM SCORE DISTRIBUTION]")
    bins = [0.0, 0.06, 0.08, 0.10, 0.16, 0.30, 0.50, 1.01]
    labels = ["0.00-0.06", "0.06-0.08", "0.08-0.10", "0.10-0.16", "0.16-0.30", "0.30-0.50", "0.50+    "]
    if correct_count is not None and 0 < correct_count < total:
        w(f"  {'Range':>10}  {'Correct':>7}  {'Wrong':>7}")
        w(f"  {'-'*10}  {'-'*7}  {'-'*7}")
        for j in range(len(labels)):
            lo, hi = bins[j], bins[j + 1]
            cc = sum(1 for c in correct if lo <= c["best_sigfm"] < hi)
            wc = sum(1 for c in wrong if lo <= c["best_sigfm"] < hi)
            bar_c = "#" * cc
            bar_w = "x" * wc
            w(f"  {labels[j]:>10}  {cc:>4} {bar_c:<3}  {wc:>4} {bar_w}")
    else:
        w(f"  {'Range':>10}  {'Count':>5}")
        w(f"  {'-'*10}  {'-'*5}")
        for j in range(len(labels)):
            lo, hi = bins[j], bins[j + 1]
            cnt = sum(1 for c in cycles if lo <= c["best_sigfm"] < hi)
            bar = "#" * cnt
            w(f"  {labels[j]:>10}  {cnt:>5} {bar}")
    w("")

    return "\n".join(out)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Generate CB2000 verify report from run logs."
    )
    ap.add_argument(
        "--run",
        default=None,
        help="Run directory (absolute path) or run ID under the current run root",
    )
    ap.add_argument(
        "--logs-root",
        type=pathlib.Path,
        default=DEFAULT_LOGS_ROOT,
        help=f"Run root directory (default: {DEFAULT_LOGS_ROOT})",
    )
    ap.add_argument(
        "--correct-count",
        type=int,
        default=None,
        help="Number of initial cycles that used the correct finger",
    )
    ap.add_argument(
        "--cutoff-time",
        default=None,
        help="Timestamp of last correct-finger verify (e.g. 11:11:36.664)",
    )
    ap.add_argument(
        "--no-save",
        action="store_true",
        help="Print report to stdout only, do not save to file",
    )
    ap.add_argument(
        "--driver-tag",
        default=DEFAULT_REPORT_DRIVER_TAG,
        help=f"Driver tag label shown in report header (default: {DEFAULT_REPORT_DRIVER_TAG})",
    )
    args = ap.parse_args()

    try:
        run_dir = resolve_run_dir(args.run, args.logs_root)
    except FileNotFoundError as e:
        print(f"ERROR: {e}", file=sys.stderr)
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
        print(f"ERROR: no verify decisions found in {run_dir}", file=sys.stderr)
        return 1

    gate_info = extract_gate_info(lines)

    # Determine correct_count from --cutoff-time if given
    correct_count = args.correct_count
    if correct_count is None and args.cutoff_time:
        for i, c in enumerate(cycles):
            if c["ts"] == args.cutoff_time:
                correct_count = i + 1
                break
        if correct_count is None:
            print(
                f"WARNING: cutoff time '{args.cutoff_time}' not found in cycles, "
                f"no split will be applied",
                file=sys.stderr,
            )

    report = format_report(run_dir, cycles, gate_info, correct_count, args.driver_tag)

    # Print to stdout
    print(report)

    # Save to file
    if not args.no_save:
        report_path = run_dir / "report.txt"
        report_path.write_text(report + "\n", encoding="utf-8")
        print(f"\nReport saved to: {report_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
