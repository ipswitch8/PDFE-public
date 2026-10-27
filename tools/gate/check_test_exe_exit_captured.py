#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""A workflow step that runs a test binary must CAPTURE its exit code.

GitHub #288, the seventh site. Three steps in windows-validation.yml invoked a
test binary directly and never read its exit code:

    & $testExe --gtest_filter="$filter" "--gtest_output=xml:$parityXml"
    if (-not (Test-Path $parityXml)) { ... }          # proves the FILE arrived
    python ... check_junit_xml.py $parityXml           # CLOBBERS $LASTEXITCODE
    if ($LASTEXITCODE -ne 0) { ... }                   # reads PYTHON's code

The signal existed for exactly one statement. `Test-Path` proves the file was
written; it cannot prove the process survived writing it. So a teardown crash --
every test passed, the artifact is complete and valid, the process then died --
left a perfect-looking artifact and passed the gate. That is the whole #288
class, and it had been fixed five times inside run_gtest.py while three CI steps
reproduced it in PowerShell.

WHY A GUARD AND NOT JUST THE FIX
--------------------------------
#288 was reported fixed three times and was not. Each enumeration was correct
and scoped one level too narrowly: the sharded path, then sites that *discard*
`rc`, then verdict sites *in this process*. The seventh site was in a different
language in a different file. Nothing lexical would have found it, and nothing
stops an eighth from being written next week -- so the property gets a standing
control rather than another round of careful reading.

THE RULE
--------
A line that directly invokes a test binary must be followed -- on the next
meaningful line, before anything else can run -- by an assignment capturing
$LASTEXITCODE. "Next" is the point: PowerShell's $LASTEXITCODE is overwritten by
the next external command, so a capture three lines later is not a capture.

Invocations through `run_gtest.py` are deliberately NOT flagged: that script
classifies the code itself (_is_abnormal_exit) and is the preferred route.

Run:  python tools/gate/check_test_exe_exit_captured.py
      python tools/gate/check_test_exe_exit_captured.py --self-test
"""

from __future__ import annotations

import argparse
import io
import os
import re
import sys
import tempfile

#: Literal tokens that mean "this line starts a test-binary process".
#: OpenCppCoverage is included because it WRAPS pdfe_tests.exe -- site 2 of 3.
#:
#: THE POPULATION IS PART OF THE SPEC. A hand-typed list is the C-ENUM defect
#: GitHub #254 and #275 exist for, and this repo has already paid for it once:
#: check_win87_publish_shapes.py's SHAPES grew by one entry after a gate pointed
#: out that the shape actually EXPLOITED was the only one missing. A list grows;
#: the property stays unmeasured. It would be incoherent for a guard written to
#: police "the signal was never captured" to decide its own scope from three
#: strings somebody remembered.
#:
#: So variable names are DERIVED from the workflow itself (see
#: _derived_binary_vars): any `$x = <...test exe...>` assignment puts `$x` in
#: scope for that file, so renaming $testExe, or adding $asanTestExe, is covered
#: without editing this file.
#:
#: HONEST RESIDUE: the two literal .exe names below are still typed, because
#: "an executable that runs tests" is not derivable from a workflow's text
#: without a model of what each binary does -- the same limitation #254 states
#: plainly rather than pretends to have solved. The derivation closes the case
#: that actually recurs (a new or renamed variable); the literals cover the
#: direct-invocation case.
LITERAL_BINARY_TOKENS = (
    "pdfe_tests.exe",
    "OpenCppCoverage.exe",
)

#: `$name = "...pdfe_tests.exe"` / `Join-Path ... "x_tests.exe"` etc. Matches an
#: assignment whose right-hand side mentions a test executable.
ASSIGN_RE = re.compile(
    r"^\s*\$(\w+)\s*=\s*(?P<rhs>.*?)\s*$")

#: What makes a right-hand side "a test executable".
TEST_EXE_RE = re.compile(r"[\w.-]*tests?\.exe", re.IGNORECASE)


def _derived_binary_vars(text: str) -> set[str]:
    """PowerShell variables bound to a test executable, read from the file.

    Derived rather than typed: a workflow that renames $testExe, or introduces
    $asanTestExe for a second lane, is covered the moment it does so. Returns
    the `$`-prefixed spellings so callers can substring-match a statement.
    """
    found: set[str] = set()
    for line in text.splitlines():
        if _is_comment_or_blank(line):
            continue
        m = ASSIGN_RE.match(line)
        if not m:
            continue
        if TEST_EXE_RE.search(m.group("rhs")):
            found.add("$" + m.group(1))
    return found

#: A PowerShell call operator at the start of a statement.
CALL_RE = re.compile(r"^\s*&\s")

#: `$something = $LASTEXITCODE`, which is the only accepted capture form.
#: Deliberately strict: `if ($LASTEXITCODE -ne 0)` is NOT a capture, because the
#: value is consumed in place and gone before the next check needs it.
CAPTURE_RE = re.compile(r"^\s*\$\w+\s*=\s*\$LASTEXITCODE\s*$")

#: Invocations routed through the runner, which handles its own classification.
EXEMPT_SUBSTRINGS = ("run_gtest.py", "run_ui_suite.py")


def _is_comment_or_blank(line: str) -> bool:
    s = line.strip()
    return (not s) or s.startswith("#")


def _continues(line: str) -> bool:
    """True if this PowerShell line continues onto the next (trailing backtick).

    Site 2 of 3 spans four lines this way, so a checker that only looked at the
    invocation line itself would demand the capture in the middle of the call.
    """
    return line.rstrip().endswith("`")


def _find_violations(text: str, path: str) -> list[str]:
    # Scope = the literals plus whatever THIS file binds to a test exe.
    tokens = tuple(LITERAL_BINARY_TOKENS) + tuple(_derived_binary_vars(text))
    lines = text.splitlines()
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if _is_comment_or_blank(line) or not CALL_RE.search(line):
            i += 1
            continue
        # Collect the whole (possibly continued) statement.
        stmt_lines = [line]
        j = i
        while _continues(lines[j]) and j + 1 < len(lines):
            j += 1
            stmt_lines.append(lines[j])
        stmt = "\n".join(stmt_lines)

        if any(tok in stmt for tok in EXEMPT_SUBSTRINGS):
            i = j + 1
            continue
        if not any(tok in stmt for tok in tokens):
            i = j + 1
            continue

        # The next MEANINGFUL line must capture the code. Comments are skipped
        # on purpose -- the fix for this very defect is heavily commented, and a
        # rule that rejected explanation would be a rule nobody keeps.
        k = j + 1
        while k < len(lines) and _is_comment_or_blank(lines[k]):
            k += 1
        captured = k < len(lines) and CAPTURE_RE.match(lines[k])
        if not captured:
            nxt = lines[k].strip() if k < len(lines) else "<end of file>"
            out.append(
                f"{path}:{i + 1}: invokes a test binary but does not capture "
                f"its exit code on the next line.\n"
                f"    invocation : {line.strip()[:110]}\n"
                f"    next line  : {nxt[:110]}\n"
                f"    Add `$someExit = $LASTEXITCODE` IMMEDIATELY after, then "
                f"test it. PowerShell overwrites $LASTEXITCODE on the next "
                f"external command, so a later read gets that command's code "
                f"and the binary's own is gone (GitHub #288). An artifact can "
                f"be complete and valid and still come from a process that "
                f"died writing it."
            )
        i = j + 1
    return out


def _workflow_files(repo_root: str) -> list[str]:
    wf_dir = os.path.join(repo_root, ".github", "workflows")
    if not os.path.isdir(wf_dir):
        return []
    return sorted(
        os.path.join(wf_dir, n) for n in os.listdir(wf_dir)
        if n.endswith((".yml", ".yaml"))
    )


def check(repo_root: str) -> int:
    files = _workflow_files(repo_root)
    if not files:
        print("[test-exe-exit] ERROR: no workflow files found under "
              ".github/workflows -- refusing to report a vacuous pass.")
        return 2
    violations: list[str] = []
    scanned = 0
    for path in files:
        try:
            text = io.open(path, encoding="utf-8").read()
        except OSError as exc:
            print(f"[test-exe-exit] ERROR: cannot read {path}: {exc}")
            return 2
        scanned += 1
        rel = os.path.relpath(path, repo_root).replace("\\", "/")
        violations.extend(_find_violations(text, rel))

    print(f"[test-exe-exit] scanned {scanned} workflow file(s) for direct "
          f"test-binary invocations.")
    if violations:
        print(f"[test-exe-exit] FAIL -- {len(violations)} invocation(s) discard "
              f"the exit code:")
        for v in violations:
            print("  " + v)
        return 1
    print("[test-exe-exit] OK -- every direct test-binary invocation captures "
          "$LASTEXITCODE on the next line.")
    return 0


# --------------------------------------------------------------------------
# Self-test. Both directions, because an all-RED set proves only sensitivity
# and an all-GREEN set proves only that nothing fires.
# --------------------------------------------------------------------------

_BAD = """\
      - name: parity
        run: |
          $testExe = "$buildBin\\pdfe_tests.exe"
          & $testExe --gtest_filter="$filter" "--gtest_output=xml:$parityXml"
          if (-not (Test-Path $parityXml)) { exit 1 }
          python tools/gate/check_junit_xml.py $parityXml
          if ($LASTEXITCODE -ne 0) { exit 1 }
"""

_GOOD = """\
      - name: parity
        run: |
          $testExe = "$buildBin\\pdfe_tests.exe"
          & $testExe --gtest_filter="$filter" "--gtest_output=xml:$parityXml"
          $parityExit = $LASTEXITCODE
          if ($parityExit -ne 0 -and $parityExit -ne 1) { exit 1 }
"""

_GOOD_CONTINUED = """\
      - name: coverage
        run: |
          & OpenCppCoverage.exe --quiet --export_type "cobertura:$cov" `
            --sources "$env:GITHUB_WORKSPACE\\src" `
            -- "$buildBin\\pdfe_tests.exe" --gtest_brief=1
          $covRunExit = $LASTEXITCODE
"""

_BAD_CONTINUED = """\
      - name: coverage
        run: |
          & OpenCppCoverage.exe --quiet --export_type "cobertura:$cov" `
            --sources "$env:GITHUB_WORKSPACE\\src" `
            -- "$buildBin\\pdfe_tests.exe" --gtest_brief=1
          python tools/gate/check_coverage_floor.py $cov
          $covExit = $LASTEXITCODE
"""

_GOOD_COMMENTED = """\
      - name: parity
        run: |
          $testExe = "$buildBin\\pdfe_tests.exe"
          & $testExe --gtest_filter=PerfBenchmark.*
          # A comment between the call and the capture is fine; the fix for
          # this defect is heavily commented and must stay legal.
          $perfRunExit = $LASTEXITCODE
"""

_EXEMPT_RUNNER = """\
      - name: full suite
        run: |
          python tests/scripts/run_gtest.py --exe $testExe --output-dir out
          echo done
"""

_BAD_IF_ONLY = r"""
      - name: parity
        run: |
          $testExe = "$buildBin\\pdfe_tests.exe"
          & $testExe --gtest_filter=Foo.*
          if ($LASTEXITCODE -ne 0) { exit 1 }
"""

#: The point of deriving the population instead of typing it: a variable this
#: file has never heard of is in scope the moment a workflow binds it to a test
#: exe. If this case ever goes green, the guard has silently narrowed to the two
#: literal names and the C-ENUM criticism in the header applies to it again.
_BAD_RENAMED_VAR = r"""
      - name: asan lane
        run: |
          $asanTestExe = "$buildBin\pdfe_tests.exe"
          & $asanTestExe --gtest_filter=Foo.*
          python tools/gate/check_junit_xml.py out.xml
"""

#: And the negative half of the derivation: a variable bound to something that
#: is NOT a test exe must stay out of scope, or the guard creeps into policing
#: every `& $something` in the repo and gets itself turned off.
_UNRELATED_VAR = r"""
      - name: packaging
        run: |
          $installer = "$buildBin\PDFE-setup.exe"
          & $installer /S
          echo packaged
"""

_NOT_A_TEST_BINARY = """\
      - name: unrelated
        run: |
          & cmake --build . --config Debug
          echo built
"""


def self_test() -> int:
    cases = [
        ("bare-invocation-is-rejected", _BAD, 1),
        ("capture-on-next-line-passes", _GOOD, 0),
        ("continued-call-then-capture-passes", _GOOD_CONTINUED, 0),
        ("continued-call-without-capture-is-rejected", _BAD_CONTINUED, 1),
        ("comments-between-call-and-capture-are-allowed", _GOOD_COMMENTED, 0),
        ("run_gtest.py-route-is-exempt", _EXEMPT_RUNNER, 0),
        ("inline-if-on-LASTEXITCODE-is-NOT-a-capture", _BAD_IF_ONLY, 1),
        ("a-non-test-binary-is-out-of-scope", _NOT_A_TEST_BINARY, 0),
        ("a-RENAMED-var-is-derived-into-scope", _BAD_RENAMED_VAR, 1),
        ("a-var-bound-to-a-NON-test-exe-stays-out", _UNRELATED_VAR, 0),
    ]
    failures = 0
    for name, body, want in cases:
        got = len(_find_violations(body, "synthetic.yml"))
        got_norm = 1 if got else 0
        ok = got_norm == want
        print(f"  {name:48s} {'PASS' if ok else 'FAIL'}"
              f"{'' if ok else f' (wanted {want}, got {got_norm})'}")
        if not ok:
            failures += 1

    # A vacuous-input direction: no workflow dir must be an ERROR, never a pass.
    with tempfile.TemporaryDirectory() as td:
        rc = check(td)
        ok = rc == 2
        print(f"  {'no-workflows-is-an-error-not-a-pass':48s} "
              f"{'PASS' if ok else 'FAIL'}")
        if not ok:
            failures += 1

    if failures:
        print(f"[test-exe-exit] self-test FAILED: {failures} case(s).")
        return 1
    print(f"[test-exe-exit] self-test OK: {len(cases) + 1} cases. A bare call, "
          f"a continued bare call, an inline `if` on $LASTEXITCODE, and a "
          f"RENAMED variable this file has never heard of all read as "
          f"violations; a capture on the next line, a capture after comments, "
          f"the run_gtest.py route, a non-test binary and a variable bound to a "
          f"non-test exe all read as clean; and an empty workflow directory is "
          f"an error rather than a vacuous pass. The renamed-variable case is "
          f"the one that proves the population is DERIVED and not just the two "
          f"literal names.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=None,
                    help="repo root (default: two levels up from this file)")
    ap.add_argument("--self-test", action="store_true",
                    help="run the built-in RED and GREEN directions")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    # THREE levels: this file is <root>/tools/gate/<name>.py. Two levels lands
    # on <root>/tools, where there is no .github/ -- which the vacuity check in
    # check() caught as an ERROR on the first run rather than reporting a
    # cheerful pass over zero files. That is the only reason this is right.
    root = args.repo_root or os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    return check(root)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
