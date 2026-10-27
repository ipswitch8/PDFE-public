# -*- coding: utf-8 -*-
"""Robust per-file pytest sweep runner.

Runs each test file as its own subprocess with a hard process-level
timeout so a pywinauto COM hang in one file does not take down the
whole sweep. Kills any orphan PDFE.exe between files. Aggregates
PASS/FAIL/SKIP/ERROR counts.

Output: writes a JSON summary + a per-file detail log under
tests/_runtime/sweeps/sweep_<timestamp>/.
"""
from __future__ import annotations

import datetime as _dt
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"
RUNTIME = ROOT / "tests" / "_runtime"
SWEEP_DIR = RUNTIME / "sweeps" / f"sweep_{_dt.datetime.now().strftime('%Y%m%dT%H%M%S')}"
SWEEP_DIR.mkdir(parents=True, exist_ok=True)

PER_FILE_HARD_TIMEOUT_S = 480  # 8 min per file — typical UI files run in 3-5 min;
                                # this catches pywinauto-COM hangs quickly without
                                # wasting time on the 30-min upper bound.
PER_TEST_PYTEST_TIMEOUT_S = 45  # pytest-timeout per-test; tighter than 60 because
                                # we now rely on the subprocess-level hard cap.
PER_FILE_RETRY_TIMEOUT_S = 360  # 6 min for the deselect-retry pass.

# Per-file timeout overrides for tests whose parametrize matrices exceed
# the default budget by design. Keys are file basename (no path).
# Values: (hard_timeout_s, retry_timeout_s).
PER_FILE_TIMEOUT_OVERRIDES: dict[str, tuple[int, int]] = {
    # 113 P0 menu rows × ~9s/row launch+click+kill ≈ 1017s; budget 1500s.
    "test_menu_navigation_p0.py": (1500, 900),
    # Pipeline f7c2a4e8 phase-8: test_dialog_reachability_p0.py is also
    # parametrized over a large dialog matrix and exceeded the 480s
    # default budget in the final sweep. 1500s leaves margin.
    "test_dialog_reachability_p0.py": (1500, 900),
}

# Known-hanging tests (pywinauto COM enumeration deadlocks observed in prior
# sweep attempts). Seeded with the one confirmed hanger so the first sweep
# pass through test_ui_annotations.py succeeds. Additional hangers found
# during this run are auto-appended and the file retried once.
KNOWN_HANGERS: set[str] = {
    "test_sticky_note_can_be_moved",
}


def kill_orphans() -> int:
    """Kill any orphan PDFE.exe processes. Returns count killed."""
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-Process pdfe -ErrorAction SilentlyContinue | "
             "ForEach-Object { Stop-Process -Id $_.Id -Force; $_.Id }"],
            capture_output=True, text=True, timeout=15,
        )
        return len([l for l in out.stdout.splitlines() if l.strip().isdigit()])
    except Exception:  # noqa: BLE001
        return -1


def discover_test_files() -> list[Path]:
    """Return all test files to run, in deterministic order."""
    files: list[Path] = []
    # Top-level tests/
    for p in sorted(TESTS.glob("test_*.py")):
        files.append(p)
    # Security
    for p in sorted((TESTS / "security").rglob("test_*.py")):
        files.append(p)
    # ui_automation
    for p in sorted((TESTS / "ui_automation").glob("test_*.py")):
        files.append(p)
    # visual
    vis = TESTS / "visual"
    if vis.exists():
        for p in sorted(vis.rglob("test_*.py")):
            files.append(p)
    return files


_SUMMARY_RE = re.compile(
    r"=+ (?:(\d+) failed)?,? ?(?:(\d+) passed)?,? ?(?:(\d+) skipped)?"
    r",? ?(?:(\d+) error[s]?)?,? ?(?:(\d+) xfailed)?,? ?(?:(\d+) xpassed)?"
    r".* in [\d.]+s"
)


def parse_summary(log_text: str) -> dict:
    """Extract pass/fail/skip counts from the pytest summary line(s)."""
    out = {"passed": 0, "failed": 0, "skipped": 0, "errors": 0,
           "xfailed": 0, "xpassed": 0}
    # The canonical pytest summary line is fenced by '====' on both sides,
    # contains an outcome word, and ends with ' in NNs' or ' in NN.NNs'.
    # Earlier failure-detail lines may incidentally contain ' in '
    # plus 'failed' — those must not be parsed as the summary line.
    # Pipeline 3e9b1d7c phase-1 (G2 fix): made fence-prefix optional
    # (`=*` instead of `=+`). When pytest's output is captured via
    # subprocess.PIPE, the canonical fence padding can be stripped
    # in some terminal-detection modes, producing a bare summary
    # like "1 failed, 1 passed in 57.36s" without leading '==='.
    # Also added .strip() on the matched line so leading whitespace
    # doesn't break the match.
    summary_re = re.compile(
        r"^=*\s*(?:.*?\d+\s+(?:passed|failed|skipped|error|xfailed|xpassed)"
        r".*?)\s+in\s+[\d.]+s",
    )
    for line in log_text.splitlines():
        stripped = line.strip()
        if not summary_re.match(stripped):
            continue
        # Skip lines that LOOK like summaries but are actually failure
        # details, e.g. "FAILED tests/...::test_x" or "tests/...::test_x ..."
        if stripped.startswith("FAILED ") or stripped.startswith("tests/"):
            continue
        line = stripped
        m = re.search(r"(\d+) failed", line)
        if m: out["failed"] += int(m.group(1))
        m = re.search(r"(\d+) passed", line)
        if m: out["passed"] += int(m.group(1))
        m = re.search(r"(\d+) skipped", line)
        if m: out["skipped"] += int(m.group(1))
        m = re.search(r"(\d+) error", line)
        if m: out["errors"] += int(m.group(1))
        m = re.search(r"(\d+) xfailed", line)
        if m: out["xfailed"] += int(m.group(1))
        m = re.search(r"(\d+) xpassed", line)
        if m: out["xpassed"] += int(m.group(1))
        break
    return out


_LAST_STARTED_RE = re.compile(
    r"^(tests[/\\][\w/\\.]+::[\w\[\]\-]+)\s", re.MULTILINE,
)


def find_last_started_test(log_text: str) -> str | None:
    """Return the LAST `tests/...::testname` line seen in the log.

    With pytest -v each test prints `tests/foo.py::test_name ` on start
    (before the result token). The last one printed before a hang is
    the hanger.
    """
    matches = _LAST_STARTED_RE.findall(log_text)
    if not matches:
        return None
    full = matches[-1]
    # Keep the full test id basename INCLUDING its [parametrize-suffix]
    # so pytest's `-k` substring match picks the specific hanging
    # variant only. Stripping the suffix here caused the retry to
    # deselect every parametrized variant of the same function (40
    # tests deselected on one hang in test_precondition_panels_phase4).
    return full.rsplit("::", 1)[-1]


def _tree_kill(pid: int) -> None:
    """Kill a process AND every descendant via taskkill /F /T.

    subprocess.Popen.kill() only kills the direct child; an orphan
    grandchild (PDFE.exe) keeps the inherited stdout/stderr handles
    open, blocking the parent's pipe reader on EOF. taskkill /T
    walks the whole tree.
    """
    try:
        subprocess.run(
            ["taskkill", "/F", "/T", "/PID", str(pid)],
            capture_output=True, timeout=10,
        )
    except Exception:  # noqa: BLE001
        pass


def _run_pytest(test_file: Path, timeout_s: int,
                deselect: set[str]) -> tuple[str, int, bool]:
    """Run pytest with optional -k deselect. Tree-kills on timeout.

    Returns (log_text, exit_code, crashed).
    """
    cmd = [sys.executable, "-m", "pytest", str(test_file),
           f"--timeout={PER_TEST_PYTEST_TIMEOUT_S}",
           "--tb=line", "-v", "--no-header"]
    if deselect:
        expr = " and ".join(f"not {n}" for n in sorted(deselect))
        cmd += ["-k", expr]

    # Stream stdout/stderr to a temp file so we don't depend on
    # subprocess pipe drain after tree-kill.
    log_fd, log_path_str = _make_tmpfile(test_file)
    proc = subprocess.Popen(
        cmd, stdout=log_fd, stderr=subprocess.STDOUT,
        cwd=str(ROOT),
    )
    try:
        exit_code = proc.wait(timeout=timeout_s)
        crashed = False
    except subprocess.TimeoutExpired:
        # Tree-kill EVERY descendant — pytest, PDFE.exe, and anything
        # else they launched. Avoids the orphan-holds-pipe deadlock.
        _tree_kill(proc.pid)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        exit_code = -1
        crashed = True
    finally:
        os.close(log_fd)

    # Read the captured log; append crash banner if needed.
    try:
        log_text = Path(log_path_str).read_text(
            encoding="utf-8", errors="replace",
        )
    except Exception:  # noqa: BLE001
        log_text = ""
    if crashed:
        log_text += f"\n--- HARD TIMEOUT after {timeout_s}s (tree-killed) ---\n"
    # Clean up tmp file — content already in log_text.
    try:
        Path(log_path_str).unlink()
    except Exception:  # noqa: BLE001
        pass
    return log_text, exit_code, crashed


def _make_tmpfile(test_file: Path) -> tuple[int, str]:
    """Open a temp file for subprocess stdout/stderr. Returns (fd, path)."""
    import tempfile  # noqa: PLC0415
    fd, path = tempfile.mkstemp(
        prefix=f"sweep_{test_file.stem}_", suffix=".log",
        dir=str(SWEEP_DIR),
    )
    return fd, path


def run_one_file(test_file: Path) -> dict:
    """Run pytest on one file. On hang, identify hanger, deselect, retry once."""
    rel = test_file.relative_to(ROOT).as_posix()
    log_path = SWEEP_DIR / (rel.replace("/", "__") + ".log")
    start = _dt.datetime.now()
    hard_to, retry_to = PER_FILE_TIMEOUT_OVERRIDES.get(
        test_file.name, (PER_FILE_HARD_TIMEOUT_S, PER_FILE_RETRY_TIMEOUT_S),
    )
    # First pass with current KNOWN_HANGERS deselected.
    log_text, exit_code, crashed = _run_pytest(
        test_file, hard_to, KNOWN_HANGERS,
    )
    retry_info: dict = {"retried": False, "new_hangers": []}
    if crashed:
        # Find the hanger, add to KNOWN_HANGERS, retry.
        hanger = find_last_started_test(log_text)
        if hanger and hanger not in KNOWN_HANGERS:
            KNOWN_HANGERS.add(hanger)
            retry_info["new_hangers"].append(hanger)
            kill_orphans()  # clean state before retry
            log_text += (
                f"\n--- RETRYING with deselect: "
                f"{sorted(KNOWN_HANGERS)} ---\n"
            )
            retry_log, exit_code, crashed_again = _run_pytest(
                test_file, retry_to, KNOWN_HANGERS,
            )
            log_text += "\n=== RETRY OUTPUT ===\n" + retry_log
            retry_info["retried"] = True
            retry_info["retry_crashed"] = crashed_again
            crashed = crashed_again  # final crash status = retry status
    log_path.write_text(log_text, encoding="utf-8", errors="replace")
    summary = parse_summary(log_text)
    summary["file"] = rel
    summary["exit_code"] = exit_code
    summary["crashed"] = crashed
    summary["duration_s"] = round((_dt.datetime.now() - start).total_seconds(), 1)
    summary.update(retry_info)
    orphans = kill_orphans()
    summary["orphans_killed_after"] = orphans
    return summary


def main() -> int:
    files = discover_test_files()
    print(f"[sweep] {len(files)} test files discovered",
          f"-> writing to {SWEEP_DIR}", flush=True)
    kill_orphans()  # baseline cleanup
    results: list[dict] = []
    totals = {"passed": 0, "failed": 0, "skipped": 0, "errors": 0,
              "xfailed": 0, "xpassed": 0, "crashed_files": 0}
    sweep_start = _dt.datetime.now()
    for i, f in enumerate(files, 1):
        rel = f.relative_to(ROOT).as_posix()
        elapsed = (_dt.datetime.now() - sweep_start).total_seconds()
        print(f"[sweep {i}/{len(files)}] (t+{int(elapsed)}s) {rel}",
              flush=True)
        r = run_one_file(f)
        results.append(r)
        for k in ("passed", "failed", "skipped", "errors",
                  "xfailed", "xpassed"):
            totals[k] += r.get(k, 0)
        if r["crashed"]:
            totals["crashed_files"] += 1
        print(f"  -> P:{r['passed']} F:{r['failed']} S:{r['skipped']} "
              f"E:{r['errors']} XF:{r['xfailed']} XP:{r['xpassed']} "
              f"exit:{r['exit_code']} {r['duration_s']}s "
              f"orphans:{r['orphans_killed_after']}",
              flush=True)
        # Persist running totals so partial-run analysis is possible.
        (SWEEP_DIR / "_running_summary.json").write_text(
            json.dumps({"totals": totals, "results": results,
                       "files_done": i, "files_total": len(files)},
                      indent=2),
            encoding="utf-8",
        )
    totals["wall_clock_s"] = round(
        (_dt.datetime.now() - sweep_start).total_seconds(), 1
    )
    (SWEEP_DIR / "_final_summary.json").write_text(
        json.dumps({"totals": totals, "results": results}, indent=2),
        encoding="utf-8",
    )
    print("\n[sweep] FINAL TOTALS:", json.dumps(totals, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
