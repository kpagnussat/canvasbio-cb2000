#!/usr/bin/env python3
"""
CB2000 battery session report — GAR / FP analysis with optional cut time.

The verify.log emits bare "MATCH!" or "NO MATCH" lines (no timestamp).
We resolve timestamps by carrying the last-seen timestamp from preceding lines.

Usage:
  cb2000_battery_report.py                          # latest session, no cut
  cb2000_battery_report.py --session 20260301_170555
  cb2000_battery_report.py --session 20260301_170555 --cut 17:07:30
  cb2000_battery_report.py --cut 17:07:30           # latest session, with cut
  cb2000_battery_report.py --sigfm                  # show per-NO-MATCH SIGFM breakdown
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

from cb2000_paths import DEFAULT_SESSION_LOGS_ROOT

LOGS_ROOT = DEFAULT_SESSION_LOGS_ROOT

TS_RE   = re.compile(r'(\d{2}:\d{2}:\d{2}\.\d+)')
ENROLL_RETRY_RE = re.compile(r"\[\s*RETRY\s*\]\s+action=enroll\b")
SIGFM_RE = re.compile(
    r'gallery=(\d+)\s+score=([\d.]+)\s+match=(\d+)\s+kp=\([^)]+\)\s+'
    r'matches=(\d+)\s+uniq=(\d+)\s+consensus=(\d+)\s+pairs=(\d+)/(\d+)'
)


def latest_session() -> pathlib.Path:
    sessions = sorted(LOGS_ROOT.iterdir())
    for s in reversed(sessions):
        if (s / "verify.log").exists():
            return s
    sys.exit("No session with verify.log found")


def parse_verdicts(log_text: str):
    """Return list of (timestamp_str, verdict_str, sigfm_block_lines)."""
    lines = log_text.splitlines()
    last_ts = ""
    pending_sigfm: list[str] = []
    results: list[tuple[str, str, list[str]]] = []

    for line in lines:
        m = TS_RE.search(line)
        if m:
            last_ts = m.group(1)

        if "[ SIGFM ]" in line:
            pending_sigfm.append(line)

        stripped = line.strip()
        if stripped in ("MATCH!", "NO MATCH!"):
            results.append((last_ts, stripped, pending_sigfm[:]))
            pending_sigfm = []

    return results


def sigfm_breakdown(sigfm_lines: list[str]) -> str:
    hits = []
    for sl in sigfm_lines:
        m = SIGFM_RE.search(sl)
        if m:
            g, score, match, raw, uniq, cons, pairs, pairs_tot = m.groups()
            hits.append(
                f"g{g}: raw={raw} uniq={uniq} pairs={pairs}/{pairs_tot} cons={cons} score={score}"
            )
    return "\n      ".join(hits) if hits else "(no SIGFM lines)"


def report(session_dir: pathlib.Path, cut: str | None, show_sigfm: bool) -> None:
    verify_log = session_dir / "verify.log"
    if not verify_log.exists():
        sys.exit(f"verify.log not found in {session_dir}")

    text = verify_log.read_text(errors="replace")
    verdicts = parse_verdicts(text)

    if not verdicts:
        print("No MATCH!/NO MATCH lines found in verify.log")
        return

    # Enroll retries
    enroll_log = session_dir / "enroll.log"
    enroll_retries = 0
    if enroll_log.exists():
        enroll_text = enroll_log.read_text(errors="replace")
        enroll_retries = len(ENROLL_RETRY_RE.findall(enroll_text))

    # Split at cut
    if cut:
        before = [(ts, v, sf) for ts, v, sf in verdicts if ts[:len(cut)] <= cut]
        after  = [(ts, v, sf) for ts, v, sf in verdicts if ts[:len(cut)] > cut]
    else:
        before = verdicts
        after  = []

    def stats(group):
        tp = sum(1 for _, v, _ in group if v == "MATCH!")
        fp = sum(1 for _, v, _ in group if v == "NO MATCH!")
        return tp, fp, len(group)

    tp_b, fp_b, tot_b = stats(before)
    tp_a, fp_a, tot_a = stats(after)
    tp_t, fp_t, tot_t = stats(verdicts)

    print(f"Session : {session_dir.name}")
    print(f"Enroll retries: {enroll_retries}")
    print()

    if cut:
        gar_b = f"{100*tp_b/tot_b:.1f}%" if tot_b else "N/A"
        print(f"BEFORE CUT {cut}")
        print(f"  MATCH!   : {tp_b}")
        print(f"  NO MATCH : {fp_b}")
        print(f"  Total    : {tot_b}")
        print(f"  GAR      : {tp_b}/{tot_b} = {gar_b}")
        print()
        print(f"AFTER CUT")
        print(f"  MATCH!   : {tp_a}")
        print(f"  NO MATCH : {fp_a}")
        print(f"  Total    : {tot_a}")
        print()

    gar_t = f"{100*tp_t/tot_t:.1f}%" if tot_t else "N/A"
    print(f"FULL SESSION")
    print(f"  MATCH!   : {tp_t}")
    print(f"  NO MATCH : {fp_t}")
    print(f"  Total    : {tot_t}")
    print(f"  GAR      : {tp_t}/{tot_t} = {gar_t}")
    print()

    # NO MATCH breakdown
    no_match_cases = [(ts, v, sf) for ts, v, sf in (before if cut else verdicts) if v == "NO MATCH!"]
    if no_match_cases:
        print(f"NO MATCH details ({len(no_match_cases)} cases):")
        for i, (ts, _, sf) in enumerate(no_match_cases, 1):
            print(f"  [{i}] {ts}")
            if show_sigfm:
                print(f"      {sigfm_breakdown(sf)}")
    else:
        print("No NO MATCH cases" + (f" before cut {cut}" if cut else "") + ".")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--session", help="Session ID (e.g. 20260301_170555); default: latest")
    ap.add_argument("--cut", help="HH:MM:SS cutoff (inclusive) for GAR window")
    ap.add_argument("--sigfm", action="store_true", help="Show per-gallery SIGFM breakdown for NO MATCH")
    args = ap.parse_args()

    if args.session:
        session_dir = LOGS_ROOT / args.session
        if not session_dir.is_dir():
            sys.exit(f"Session dir not found: {session_dir}")
    else:
        session_dir = latest_session()

    report(session_dir, args.cut, args.sigfm)


if __name__ == "__main__":
    main()
