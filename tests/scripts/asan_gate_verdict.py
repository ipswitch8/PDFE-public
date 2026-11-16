#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Decide PASS/FAIL for an ASan run, from artifacts only -- cross-platform.

A line-for-line port of tests/scripts/asan_gate_verdict.ps1 (SEC-21), so that
Linux and macOS can drive the same verdict logic without PowerShell. The
reasoning behind every rule lives in the .ps1 header and DCA 0029; it is not
repeated here. The two must not drift:
tests/test_sanitizer_gate_fault_injection.cpp runs this script on POSIX, and on
Windows it runs BOTH and requires identical verdicts for every fixture case
(SanitizerGateFaultInjection.ThePythonAndPowerShellGatesAgree).

The command line mirrors the .ps1 parameters (-StdoutLog, -LogDir, -LogBase,
-XmlPath, -Rc, -CollectTo). -LogDir may be repeated, or given a
comma-separated list as PowerShell accepts. Output is a single compact JSON
object on stdout with the same keys, in the same order. Exit code 0 means
PASS and 1 means FAIL.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

# ASan prints one of these per finding, in both stderr and log_path files.
FINDING = re.compile(r"==\d+==ERROR:")
OPTIONS_ERROR = re.compile(r"in ASAN_OPTIONS", re.IGNORECASE)


def matching_lines(path: str, pattern: re.Pattern) -> list[str]:
    """Lines of `path` that match, as Select-String counts them (per line).
    Select-String matches case-insensitively by default; so does this."""
    pat = re.compile(pattern.pattern, re.IGNORECASE)
    with open(path, encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\r\n") for line in f if pat.search(line)]


def parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prefix_chars="-", allow_abbrev=False,
                                 description=__doc__.splitlines()[0])
    ap.add_argument("-StdoutLog", default="")
    ap.add_argument("-LogDir", action="append", default=[])
    ap.add_argument("-LogBase", default="asan_ci_finding")
    ap.add_argument("-XmlPath", default="")
    ap.add_argument("-Rc", type=int, default=0)
    ap.add_argument("-CollectTo", default="")
    args = ap.parse_args(argv)
    dirs: list[str] = []
    for d in args.LogDir:
        dirs.extend(part for part in d.split(","))
    args.LogDir = dirs
    return args


def main(argv: list[str] | None = None) -> int:
    a = parse_args(sys.argv[1:] if argv is None else argv)
    rc = a.Rc
    diagnostics: list[str] = []

    # --- Source 1: the merged stdout capture. ---------------------------
    findings_stdout = 0
    stdout_exists = bool(a.StdoutLog) and os.path.exists(a.StdoutLog)
    if stdout_exists:
        findings_stdout = len(matching_lines(a.StdoutLog, FINDING))
    elif a.StdoutLog:
        diagnostics.append(f"stdout log not found at '{a.StdoutLog}'")

    # --- Source 2: log_path files, across every candidate directory. ----
    seen: set[str] = set()
    log_files: list[str] = []
    findings_log_files = 0
    for d in a.LogDir:
        if not d or not os.path.exists(d):
            continue
        full = os.path.abspath(d)
        if full in seen:
            continue
        seen.add(full)
        try:
            names = sorted(os.listdir(full))
        except OSError:
            continue
        prefix = a.LogBase + "."
        for name in names:
            p = os.path.join(full, name)
            if name.startswith(prefix) and os.path.isfile(p):
                log_files.append(p)
                findings_log_files += len(matching_lines(p, FINDING))

    findings = findings_stdout + findings_log_files

    # --- ASAN_OPTIONS parse failure means the tests never ran at all. ---
    if stdout_exists:
        opt_err = matching_lines(a.StdoutLog, OPTIONS_ERROR)
        if opt_err:
            diagnostics.append(f"ASAN_OPTIONS parse error - tests did NOT run: {opt_err[0]}")
            rc = 1

    # --- GAP-A15 mirror: a green exit code over a red XML. --------------
    gtest_failures = 0
    if a.XmlPath:
        if os.path.exists(a.XmlPath):
            try:
                root = ET.parse(a.XmlPath).getroot()
                suites = root if root.tag == "testsuites" else root.find("testsuites")
                if suites is None:
                    raise ValueError("no <testsuites> element")
                gtest_failures = int(suites.get("failures", 0) or 0) + \
                                 int(suites.get("errors", 0) or 0)
            except (ET.ParseError, ValueError) as exc:
                diagnostics.append(
                    f"results XML at '{a.XmlPath}' could not be parsed: {exc}")
                rc = 1
            if gtest_failures > 0:
                diagnostics.append(
                    f"gtest reported {gtest_failures} failure(s)/error(s) in {a.XmlPath}")
                rc = 1
        else:
            diagnostics.append(
                f"results XML not produced at '{a.XmlPath}' - cannot verify subset results")
            rc = 1

    verdict = "PASS" if rc == 0 and findings == 0 else "FAIL"

    # --- The evidence cross-check. --------------------------------------
    evidence_collected = True
    if rc != 0 and findings == 0:
        evidence_collected = False
        diagnostics.append(
            f"EVIDENCE GAP: process exited {rc} but zero ASan findings were collected. "
            "ASan resolves a relative log_path against the PROCESS CWD; if the run's "
            "CWD was not among the scanned directories ("
            + "; ".join(d for d in a.LogDir if d)
            + ") the report exists but was never read. Do not triage this as a flake.")

    if a.CollectTo and log_files:
        os.makedirs(a.CollectTo, exist_ok=True)
        for lf in log_files:
            target = os.path.join(a.CollectTo, os.path.basename(lf))
            if os.path.abspath(lf) != os.path.abspath(target):
                try:
                    shutil.move(lf, target)
                except OSError:
                    pass  # -ErrorAction SilentlyContinue

    print(json.dumps({
        "verdict": verdict,
        "rc": rc,
        "findings": findings,
        "findingsStdout": findings_stdout,
        "findingsLogFiles": findings_log_files,
        "logFiles": log_files,
        "gtestFailures": gtest_failures,
        "evidenceCollected": evidence_collected,
        "diagnostics": diagnostics,
    }, separators=(",", ":"), ensure_ascii=False))
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
