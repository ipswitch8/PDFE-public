# -*- coding: utf-8 -*-
"""Pipeline a7c2e541 phase-16 testing-methodology lint: flag Python
UI tests (tests/test_ui_*.py) that perform UI actions but never
assert a post-condition proving the action took effect.

Background
----------
The PDFE Python UI layer drives pywinauto / WinAppDriver / FlaUI to
simulate clicks, menu invocations, drag operations, and keyboard
input against the running PDFE.exe. A common methodology gap is:

    def test_open_file_from_menu(...):
        click_menu("File > Open")
        select_file_in_dialog("sample.pdf")
        # <-- no assertion here that the file actually opened

A bug that breaks the open-file pipeline silently passes this test
because the actions complete without throwing — but no observable
post-condition is checked.

Detection
---------
For each `def test_*` function in any `tests/test_ui_*.py`:

1. Tally how many UI-action calls appear inside the function body
   (`click(`, `select_item`, `set_edit_text(`, `set_value(`, `keys(`,
   etc.) — see ``ACTION_PATTERNS``.
2. Tally how many post-condition assertions appear
   (`assert `, `pytest.fail`, `xfail`, `xfail()`, ``.get_value(``,
   ``window_text(``, ``.window_text() ==`` etc.) — see
   ``ASSERTION_PATTERNS``.
3. If action_count >= 1 and assertion_count == 0, the test is flagged.

Tests with `pytest.skip`, `pytest.xfail`, or `@pytest.mark.xfail` are
allowed to no-op without an assertion (skipped tests have nothing
to verify; xfail tests carry their gap in the marker itself).

Strict: bare pyautogui keystroke check
---------------------------------------
In ``--strict`` mode (the default when running in CI / gate context)
the lint also flags any Python file under ``tests/`` that contains a
bare ``pyautogui.{typewrite,press,hotkey,keyDown,keyUp}(`` call
outside the explicitly allowlisted files:

    Allowlist (not flagged):
      - tests/helpers/keys.py          — the wrapper module itself
      - tests/helpers/file_dialog.py   — screenshot capture only
      - tests/test_ui_page_operations.py — pyautogui.click in a
                                           foreground-verified try/finally
                                           block (Ctrl+click fallback,
                                           audited safe per phase-4)

Output format mirrors lint_save_without_verify.py.

Usage
-----
    python tests/scripts/lint_python_ui_gaps.py tests/
    python tests/scripts/lint_python_ui_gaps.py --strict tests/

Phase-16 acceptance criterion AC3: this lint reports 0 findings and
exits 0. Default behavior (and --strict) both hard-exit-1 on any
finding; --advisory forces exit 0 (used during initial development).
"""
from __future__ import annotations

import argparse
import ast
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


# ─── Patterns ────────────────────────────────────────────────────────

# Patterns that indicate a UI mutation took place. The full pywinauto
# / Win32 surface is wider than this — these are the patterns the
# PDFE test suite actually uses.
ACTION_PATTERNS = [
    re.compile(r"\.click\s*\("),
    re.compile(r"\.click_input\s*\("),
    re.compile(r"\.invoke\s*\("),
    re.compile(r"\.toggle\s*\("),
    re.compile(r"\.select\s*\("),
    re.compile(r"\.set_edit_text\s*\("),
    re.compile(r"\.set_value\s*\("),
    re.compile(r"\.type_keys\s*\("),
    re.compile(r"\.send_keystrokes\s*\("),
    re.compile(r"\bpyautogui\.click\s*\("),
    re.compile(r"\bpyautogui\.typewrite\s*\("),
    re.compile(r"\bpyautogui\.hotkey\s*\("),
    re.compile(r"\bpyautogui\.dragTo\s*\("),
    re.compile(r"\bpyautogui\.mouseDown\s*\("),
    re.compile(r"\bpyautogui\.mouseUp\s*\("),
    re.compile(r"\.MenuSelect\s*\("),
    re.compile(r"\.menu_select\s*\("),
    re.compile(r"\bSendInput\s*\("),
    re.compile(r"\bSetCursorPos\s*\("),
    re.compile(r"\bmouse_event\s*\("),
    re.compile(r"\bkeybd_event\s*\("),
    re.compile(r"\bPostMessage\s*\("),
    # PDFE-specific helpers
    re.compile(r"\b_apply_text_markup\s*\("),
    re.compile(r"\b_draw_small_redaction\s*\("),
    re.compile(r"\b_open_file\s*\("),
    re.compile(r"\b_click_menu_item\s*\("),
    re.compile(r"\b_drive_color_dialog_pick"),
    re.compile(r"\b_click_field_by_name\s*\("),
    re.compile(r"\b_tab_to_field_named\s*\("),
]

# Patterns that indicate the test asserted some post-condition.
ASSERTION_PATTERNS = [
    # re.MULTILINE is load-bearing and was MISSING (GitHub #163). The body
    # is scanned as one newline-joined string, so without it ``^`` anchors
    # to the start of the whole body — which is the docstring — and a bare
    # ``assert`` statement was detectable only if it happened to be the
    # very first statement of the test. In practice that meant this linter
    # has never counted a plain ``assert`` at all: its three standing
    # findings were tests with 6-9 assertions each. A detector that cannot
    # see the most common form of the thing it looks for can only ever
    # ratify, which is the whole defect class #160/#162/#171/#175 are about.
    re.compile(r"^\s*assert\b", re.MULTILINE),
    re.compile(r"\bpytest\.fail\s*\("),
    re.compile(r"\bpytest\.raises\s*\("),
    re.compile(r"\bself\.assertEqual\b"),
    re.compile(r"\bself\.assertNotEqual\b"),
    re.compile(r"\bself\.assertTrue\b"),
    re.compile(r"\bself\.assertFalse\b"),
    re.compile(r"\bself\.assertIn\b"),
    re.compile(r"\bself\.assertNotIn\b"),
    re.compile(r"\bself\.assertIsNotNone\b"),
    re.compile(r"\bself\.assertIsNone\b"),
    re.compile(r"\bself\.assertIs\b"),
    re.compile(r"\bself\.assertIsNot\b"),
    re.compile(r"\bself\.assertGreater\b"),
    re.compile(r"\bself\.assertLess\b"),
    re.compile(r"\bself\.assertGreaterEqual\b"),
    re.compile(r"\bself\.assertLessEqual\b"),
    re.compile(r"\bself\.assertAlmostEqual\b"),
    re.compile(r"\bself\.assertRaises\b"),
    re.compile(r"\bself\.fail\s*\("),
    # PDFE-specific assertion helpers (call _assert_*).
    re.compile(r"\b_assert_canvas_changed\s*\("),
    re.compile(r"\b_assert_canvas_unchanged\s*\("),
    re.compile(r"\b_assert_canvas_diff_above\s*\("),
    re.compile(r"\b_assert_in_log\s*\("),
    # Comparison readback patterns that count as observation:
    re.compile(r"\.window_text\s*\(\)\s*=="),
    re.compile(r"\.get_value\s*\(\)\s*=="),
    re.compile(r"\.is_visible\s*\(\)"),
    re.compile(r"\.is_enabled\s*\(\)"),
    re.compile(r"\.is_checked\s*\(\)"),
    re.compile(r"\.exists\s*\("),
    re.compile(r"\.children_count\s*\("),
    re.compile(r"\.SSIM\s*\("),
    re.compile(r"\bSSIM\s*="),
    re.compile(r"\bssim\s*\("),
    # File-system observables
    re.compile(r"\bos\.path\.exists\s*\("),
    re.compile(r"\bos\.path\.getsize\s*\("),
    re.compile(r"\bPath\(.*\)\.exists\s*\("),
    re.compile(r"\bPath\(.*\)\.stat\s*\("),
    # pikepdf / pymupdf inspect after-the-fact
    re.compile(r"\bpikepdf\.open\s*\("),
    re.compile(r"\bfitz\.open\s*\("),
    re.compile(r"\bextract_text\s*\("),
]

# Skip/xfail markers — tests with these markers are exempt from
# the gap requirement (they explicitly opt out of running).
SKIP_PATTERNS = [
    re.compile(r"@pytest\.mark\.skip"),
    re.compile(r"@pytest\.mark\.xfail"),
    re.compile(r"\bpytest\.skip\s*\("),
    re.compile(r"\bpytest\.xfail\s*\("),
]


@dataclass
class Finding:
    file: Path
    line: int
    test_name: str
    action_count: int


def is_skip_marked(file_text_above: str) -> bool:
    """Return True if a skip/xfail marker is set on this test (either
    via decorator above the def, or via a pytest.skip / pytest.xfail
    call inside the body)."""
    for pat in SKIP_PATTERNS:
        if pat.search(file_text_above):
            return True
    return False


def scan_test_function(file: Path, source_lines: list[str],
                       start: int, end: int,
                       decorator_text: str) -> Finding | None:
    """Scan source_lines[start:end] for action/assertion counts."""
    body = "\n".join(source_lines[start:end])
    if is_skip_marked(decorator_text) or is_skip_marked(body):
        return None
    action_count = 0
    for pat in ACTION_PATTERNS:
        action_count += len(pat.findall(body))
    if action_count == 0:
        return None
    assertion_count = 0
    for pat in ASSERTION_PATTERNS:
        assertion_count += len(pat.findall(body))
    if assertion_count > 0:
        return None
    # Test-function name is at line `start - 1` (caller passes the
    # body start, the def line is on the line above).
    m = re.match(r"^\s*def\s+(test_\w+)",
                 source_lines[start - 1] if start > 0 else "")
    name = m.group(1) if m else "<unknown>"
    return Finding(file=file, line=start, test_name=name,
                   action_count=action_count)


def lint_file(file: Path) -> list[Finding]:
    source = file.read_text(encoding="utf-8", errors="replace")
    source_lines = source.splitlines()
    findings: list[Finding] = []
    try:
        tree = ast.parse(source, filename=str(file))
    except SyntaxError:
        return findings
    for node in ast.walk(tree):
        if not isinstance(node, ast.FunctionDef):
            continue
        if not node.name.startswith("test_"):
            continue
        body_start = (node.body[0].lineno - 1) if node.body else node.lineno
        body_end = (node.end_lineno or body_start + 1)
        # Capture decorators (preceding lines from first decorator).
        deco_text = ""
        if node.decorator_list:
            deco_start = min(d.lineno for d in node.decorator_list) - 1
            deco_text = "\n".join(source_lines[deco_start:node.lineno])
        result = scan_test_function(
            file, source_lines, body_start, body_end, deco_text)
        if result is not None:
            findings.append(result)
    return findings


def lint_path(path: Path) -> list[Finding]:
    findings: list[Finding] = []
    if path.is_file() and path.suffix == ".py":
        if path.name.startswith("test_ui_"):
            return lint_file(path)
        return []
    if path.is_dir():
        for py in sorted(path.rglob("test_ui_*.py")):
            findings.extend(lint_file(py))
    return findings


def format_finding(f: Finding) -> str:
    return (f"{f.file}:{f.line}  test={f.test_name}  "
            f"actions={f.action_count}  assertions=0  "
            f"hint=add an assertion (UI-state, file-system, or visual "
            f"observable) proving the action took effect")


# ─── Bare pyautogui keystroke check (strict mode) ────────────────────


@dataclass
class KeystrokeFinding:
    file: Path
    line: int
    col: int
    text: str


# Bare pyautogui keystroke call pattern — matches:
#   pyautogui.typewrite(
#   pyautogui.press(
#   pyautogui.hotkey(
#   pyautogui.keyDown(
#   pyautogui.keyUp(
_BARE_KEYSTROKE_RE = re.compile(
    r"\bpyautogui\.(typewrite|press|hotkey|keyDown|keyUp)\s*\("
)

# Files / directories where bare pyautogui keystroke calls are
# intentionally permitted.  Allowlisted entries are matched against the
# file's POSIX-normalized path (forward slashes) using simple substring
# matching so both filename-only and path-prefix rules work.
#
# keys.py        — the focus-safe wrapper module itself (wraps pyautogui
#                  internally; this is the definition of the safe API).
# file_dialog.py — contains pyautogui.screenshot() only (not a
#                  keystroke); listed for belt-and-suspenders clarity.
# test_ui_page_operations.py — Ctrl+click fallback wrapped with
#                  ensure_foreground + verify_foreground_is guards;
#                  audited safe in Pipeline F phase-4.
#
# Helper / harness infrastructure:
#   tests/ui_harness/**   — lower-level action helpers.
#                           NOTE: tests/ui_harness/text_edit/helpers.py was
#                           audited and ALL 13 bare pyautogui keystroke sites
#                           converted to focus-safe wrappers in Pipeline F
#                           phase-5b (P-G-INFRA-2/3 remediation, 2026-05-11).
#                           The path-substring allowlist is retained because
#                           other files under /ui_harness/ (e.g. core/canvas.py,
#                           core/locate.py) do not contain bare keystroke calls
#                           and the allowlist causes no false-negatives for them.
#                           If new files are added to /ui_harness/ they must
#                           follow the focus-safe wrapper rule; only the explicit
#                           keys.py wrapper is permitted to call bare pyautogui.
#   tests/ui_automation/  — automation subsystem helpers.
#   tests/visual/         — visual baseline test infrastructure.
#   tests/helpers/qinvoke.py — queue helper (comment text only matches).
#
# Individual legacy test files that predate the focus-safety audit and
# are not in the phase-4 fix list:
_KEYSTROKE_ALLOWLIST_NAMES: set[str] = {
    "keys.py",
    "file_dialog.py",
    "qinvoke.py",
    "lint_python_ui_gaps.py",  # this file — doc comments contain examples
    "test_ui_page_operations.py",
    # test_keys_focus_safety.py — only references pyautogui in doc
    # comments (not code); kept here for belt-and-suspenders clarity.
    "test_keys_focus_safety.py",
    "test_ui_view_navigation.py",
    # test_ui_pipe_focus_theft.py (GitHub #303) — the ONE case where a bare
    # keystroke is the point of the test, not a defect in it.
    #
    # That test proves a keystroke sent immediately after a UIA pipe write is
    # not swallowed by a hidden zero-size QLineEdit. The focus-safe wrappers
    # this lint exists to promote work by asserting focus on the intended
    # target FIRST -- which is exactly the step that would mask the defect
    # under test. Rewriting those three sites to use `hotkey_to`/`press_to`
    # would make the test pass while proving nothing, which is a worse outcome
    # than the lint finding.
    #
    # Allowlisted by NAME rather than by line, so the entry cannot rot when the
    # file shifts. Noted because this guard was already red on master before
    # p97 phase-9 touched the file -- the three findings are byte-identical at
    # the prior commit -- so this closes a standing red rather than one the
    # phase introduced.
    "test_ui_pipe_focus_theft.py",
}

# Path segments — any file whose POSIX path contains one of these
# substrings is also allowlisted.
_KEYSTROKE_ALLOWLIST_PATH_SUBSTRINGS: tuple[str, ...] = (
    "/ui_harness/",
    "\\ui_harness\\",
    "/ui_automation/",
    "\\ui_automation\\",
    "/visual/",
    "\\visual\\",
)


def _is_allowlisted(file: Path) -> bool:
    if file.name in _KEYSTROKE_ALLOWLIST_NAMES:
        return True
    path_str = str(file)
    for sub in _KEYSTROKE_ALLOWLIST_PATH_SUBSTRINGS:
        if sub in path_str:
            return True
    return False


def check_bare_keystrokes(path: Path) -> list[KeystrokeFinding]:
    """Scan Python files for bare pyautogui keystroke calls.

    Returns one ``KeystrokeFinding`` per matching line.  Files in
    ``_KEYSTROKE_ALLOWLIST`` are skipped.
    """
    findings: list[KeystrokeFinding] = []
    targets: list[Path] = []

    if path.is_file() and path.suffix == ".py":
        targets = [path]
    elif path.is_dir():
        targets = sorted(path.rglob("*.py"))
    else:
        return findings

    for file in targets:
        if _is_allowlisted(file):
            continue
        try:
            source = file.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(source.splitlines(), start=1):
            m = _BARE_KEYSTROKE_RE.search(line)
            if m:
                findings.append(KeystrokeFinding(
                    file=file,
                    line=lineno,
                    col=m.start() + 1,
                    text=line.strip(),
                ))
    return findings


def format_keystroke_finding(f: KeystrokeFinding) -> str:
    return (
        f"{f.file}:{f.line}:{f.col}  BARE-PYAUTOGUI-KEYSTROKE  "
        f"{f.text!r}  "
        f"hint=replace with focus-safe wrapper from tests/helpers/keys.py "
        f"(hotkey_to / typewrite_to / press_to / send_keys_to)"
    )


def main(argv: Iterable[str]) -> int:
    ap = argparse.ArgumentParser(
        description=("Flag Python UI tests that perform actions but "
                     "do not assert a post-condition."))
    ap.add_argument("paths", nargs="+", type=Path,
                    help="Test files or directories to lint.")
    ap.add_argument("--quiet", action="store_true",
                    help="Print only the summary count.")
    ap.add_argument("--strict", action="store_true",
                    help="Hard-exit-1 on findings (default). "
                         "Also enables the bare-pyautogui-keystroke check.")
    ap.add_argument("--advisory", action="store_true",
                    help="Force exit 0 regardless of findings "
                         "(historic phase-1 behavior).")
    args = ap.parse_args(list(argv))

    findings: list[Finding] = []
    keystroke_findings: list[KeystrokeFinding] = []

    for p in args.paths:
        if not p.exists():
            print(f"ERROR: path does not exist: {p}", file=sys.stderr)
            return 2
        findings.extend(lint_path(p))
        if args.strict:
            keystroke_findings.extend(check_bare_keystrokes(p))

    if not args.quiet:
        for f in findings:
            print(format_finding(f))
        for kf in keystroke_findings:
            print(format_keystroke_finding(kf))

    total = len(findings) + len(keystroke_findings)
    print(f"\n[lint_python_ui_gaps] {len(findings)} action/assertion finding(s), "
          f"{len(keystroke_findings)} bare-keystroke finding(s), "
          f"{total} total in {len(args.paths)} path(s).")

    if args.advisory:
        return 0
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
