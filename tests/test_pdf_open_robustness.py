# -*- coding: utf-8 -*-
"""
Robustness test: open every PDF in test_PDFs/ with pdfe.exe and verify no crashes.

For each file:
  1. Launch pdfe.exe <file>
  2. Wait up to TIMEOUT seconds
  3. If process is still alive → PASS (app opened without crashing), then kill it
  4. If process exited → check exit code:
       - 0 means graceful close/error dialog → PASS
       - non-zero means crash → FAIL
"""

import os
import sys
import subprocess
import time
import signal
import json
from pathlib import Path

# Force UTF-8 on Windows console
if sys.platform == "win32":
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

# Resolve the pdfe.exe path via the harness module, which honors
# PDFE_BUILD_CONFIG for Debug/Release switching. Falls back to the
# absolute Debug path if the harness isn't importable (e.g. running
# this script from a checkout without tests/ui_automation/harness).
_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))
try:
    from tests.ui_automation.harness.pdfe_app_fixture import (
        PDFE_EXE as _HARNESS_PDFE_EXE,
        kill_pdfe_processes,
    )
    PDFE_EXE = str(_HARNESS_PDFE_EXE)
except ImportError:
    PDFE_EXE = str(
        _REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "pdfe.exe"
    )
    def kill_pdfe_processes() -> None:  # noqa: E305
        """Fallback taskkill sweeper when harness is unavailable."""
        try:
            subprocess.run(
                ["taskkill", "/F", "/IM", "pdfe.exe"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=10,
            )
        except Exception:  # noqa: BLE001
            pass

# Corpus directory — overridable via env var for CI and for
# developers who keep their test corpus somewhere else. Default
# lives at repo-root/test_PDFs/ which is gitignored.
TEST_DIR = os.environ.get(
    "PDFE_TEST_PDFS_DIR",
    str(_REPO_ROOT / "test_PDFs"),
)

# Files over this size (bytes) get a longer timeout
LARGE_FILE_THRESHOLD = 50 * 1024 * 1024  # 50 MB
DEFAULT_TIMEOUT = 10
LARGE_TIMEOUT = 30


def get_timeout_for_file(filepath: str) -> int:
    """Return timeout in seconds based on file size."""
    try:
        size = os.path.getsize(filepath)
        if size > LARGE_FILE_THRESHOLD:
            return LARGE_TIMEOUT
    except OSError:
        pass
    return DEFAULT_TIMEOUT


def test_open_pdf(filepath: str, timeout: int) -> dict:
    """
    Launch pdfe.exe with the given PDF and check if it crashes.

    Returns dict with keys: file, status, exit_code, duration, detail
    """
    result = {
        "file": os.path.basename(filepath),
        "status": "UNKNOWN",
        "exit_code": None,
        "duration": 0.0,
        "detail": "",
    }

    start = time.monotonic()
    try:
        proc = subprocess.Popen(
            [PDFE_EXE, filepath],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW,
            # Suppress the "Recover unsaved changes" modal: a prior UI test that
            # was killed while dirty may have left a recovery sidecar, which
            # would otherwise wedge this read-only launch on a startup dialog.
            env={**os.environ, "PDFE_TEST_SKIP_UNSAVED_PROMPT": "1"},
        )
    except Exception as e:
        result["status"] = "ERROR"
        result["detail"] = f"Failed to launch: {e}"
        result["duration"] = time.monotonic() - start
        return result

    try:
        # Wait for the process to either exit (crash) or stay alive (success)
        exit_code = proc.wait(timeout=timeout)
        result["duration"] = time.monotonic() - start
        result["exit_code"] = exit_code

        if exit_code == 0:
            # Graceful exit within timeout - could be error dialog closed or
            # the app decided to quit. Still counts as "no crash".
            result["status"] = "PASS"
            result["detail"] = "Exited gracefully (code 0)"
        else:
            result["status"] = "FAIL"
            # Interpret common Windows crash codes
            if exit_code < 0:
                code_hex = hex(exit_code & 0xFFFFFFFF)
            else:
                code_hex = hex(exit_code)
            detail = f"Crashed with exit code {exit_code} ({code_hex})"
            # Known Windows exception codes
            crash_codes = {
                0xC0000005: "ACCESS_VIOLATION",
                0xC0000374: "HEAP_CORRUPTION",
                0xC00000FD: "STACK_OVERFLOW",
                0xC0000409: "STACK_BUFFER_OVERRUN",
                0x80000003: "BREAKPOINT",
                0xC000001D: "ILLEGAL_INSTRUCTION",
                0xC0000094: "INTEGER_DIVIDE_BY_ZERO",
            }
            unsigned = exit_code & 0xFFFFFFFF
            if unsigned in crash_codes:
                detail += f" - {crash_codes[unsigned]}"
            result["detail"] = detail

    except subprocess.TimeoutExpired:
        # Process is still alive after timeout → it opened successfully
        result["duration"] = time.monotonic() - start
        result["status"] = "PASS"
        result["detail"] = f"Still running after {timeout}s (no crash)"

        # Kill the process gracefully
        try:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        except Exception:
            # Last resort
            try:
                subprocess.run(
                    ["taskkill", "/F", "/PID", str(proc.pid)],
                    capture_output=True,
                    timeout=5,
                )
            except Exception:
                pass

    except Exception as e:
        result["duration"] = time.monotonic() - start
        result["status"] = "ERROR"
        result["detail"] = f"Unexpected error: {e}"
        try:
            proc.kill()
        except Exception:
            pass

    return result


def main():
    if not os.path.isfile(PDFE_EXE):
        print(f"ERROR: pdfe.exe not found at {PDFE_EXE}")
        sys.exit(1)

    if not os.path.isdir(TEST_DIR):
        print(f"ERROR: test_PDFs directory not found at {TEST_DIR}")
        sys.exit(1)

    # Collect all PDF files
    pdf_files = sorted([
        os.path.join(TEST_DIR, f)
        for f in os.listdir(TEST_DIR)
        if f.lower().endswith(".pdf")
    ])

    if not pdf_files:
        print("No PDF files found in test directory.")
        sys.exit(1)

    # GitHub #189 — A CORPUS OF ONE IS NOT A CORPUS.
    #
    # The directory-exists check above passes on a CLEAN CHECKOUT, because
    # .gitignore re-includes exactly one file (crazyones-pdfa.pdf, 16 KB) out of
    # the 64 that exist on the machine that produced them. So this script would
    # "test 1 PDF file", print a clean report, and exit 0 — a robustness sweep
    # reporting success over 1.6% of its corpus, indistinguishable from a real
    # pass. The whole point of this script is breadth; without breadth it is
    # asserting nothing worth asserting.
    #
    # The floor is deliberately low and deliberately not 64: the corpus grows
    # and shrinks, and a floor pinned to its exact size would be edited to
    # whatever it happened to be. 10 is far above the 1 a clean checkout has and
    # far below any real corpus.
    MIN_CORPUS = 10
    if len(pdf_files) < MIN_CORPUS:
        print(f"ERROR: only {len(pdf_files)} PDF(s) in {TEST_DIR}; this sweep "
              f"needs at least {MIN_CORPUS} to mean anything.")
        print("       test_PDFs/ is gitignored (.gitignore:235) and only "
              "crazyones-pdfa.pdf is tracked, so a clean checkout lands here.")
        print("       This is a REFUSAL TO REPORT, not a pass: a robustness "
              "sweep over one file that says 'all clean' is worse than no "
              "sweep, because somebody will read it as coverage.")
        sys.exit(1)

    print(f"Testing {len(pdf_files)} PDF files with pdfe.exe")
    print(f"Executable: {PDFE_EXE}")
    print(f"Default timeout: {DEFAULT_TIMEOUT}s, Large file timeout: {LARGE_TIMEOUT}s")
    print("=" * 80)

    results = []
    passes = 0
    fails = 0
    errors = 0

    for i, filepath in enumerate(pdf_files, 1):
        filename = os.path.basename(filepath)
        file_size = os.path.getsize(filepath)
        timeout = get_timeout_for_file(filepath)

        size_str = f"{file_size / 1024:.0f} KB" if file_size < 1024 * 1024 else f"{file_size / (1024*1024):.1f} MB"
        print(f"[{i:3d}/{len(pdf_files)}] {filename} ({size_str}, {timeout}s timeout) ... ", end="", flush=True)

        result = test_open_pdf(filepath, timeout)
        results.append(result)

        status_marker = {
            "PASS": "OK",
            "FAIL": "CRASH",
            "ERROR": "ERR",
            "UNKNOWN": "???",
        }[result["status"]]

        print(f"{status_marker}  ({result['duration']:.1f}s) {result['detail']}")

        if result["status"] == "PASS":
            passes += 1
        elif result["status"] == "FAIL":
            fails += 1
        else:
            errors += 1

    # Summary
    print("=" * 80)
    print(f"RESULTS: {passes} passed, {fails} crashed, {errors} errors out of {len(pdf_files)} files")

    if fails > 0:
        print("\nCRASHED FILES:")
        for r in results:
            if r["status"] == "FAIL":
                print(f"  - {r['file']}: {r['detail']}")

    if errors > 0:
        print("\nERROR FILES:")
        for r in results:
            if r["status"] == "ERROR":
                print(f"  - {r['file']}: {r['detail']}")

    # Save results to JSON
    results_path = os.path.join(os.path.dirname(__file__), "pdf_open_robustness_results.json")
    with open(results_path, "w", encoding="utf-8") as f:
        json.dump({
            "summary": {
                "total": len(pdf_files),
                "passed": passes,
                "crashed": fails,
                "errors": errors,
            },
            "results": results,
        }, f, indent=2, ensure_ascii=False)
    print(f"\nDetailed results saved to: {results_path}")

    # Final sweep: if any test leaked an orphaned pdfe.exe (a
    # subprocess that didn't exit cleanly under terminate/kill),
    # the harness's taskkill fallback catches it here.
    kill_pdfe_processes()

    # Exit with failure if any crashes
    sys.exit(1 if fails > 0 else 0)


if __name__ == "__main__":
    main()
