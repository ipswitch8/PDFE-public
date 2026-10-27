# -*- coding: utf-8 -*-
"""Every canvas capture compared against a baseline must be page-cropped.

WHY THIS EXISTS (claim-check, phase-1, 2026-10-06)
==================================================
The crop that decouples the canvas oracles from the session theme was wired by
HAND into ten call sites. Nothing enforced that it reached all of them, and the
first attempt reached only five -- it searched for the baseline-name prefix
`phase5_open` instead of enumerating the `_capture_canvas(s)` call sites, so five
siblings with different names were invisible. karen caught that by running the
missed five and finding them still red.

The claim "all canvas oracles are cropped" was therefore TRUE but UNMECHANISED:
true on the day it was written, with nothing to stop an eleventh site shipping
uncropped and silently asserting the session theme again. That is the exact shape
of the defect it was introduced to fix, so it gets a guard rather than a promise.

WHAT IS AND IS NOT REQUIRED
---------------------------
Required: a `_baseline_check(...)` whose image argument came from
`_capture_canvas(...)` must pass `crop=`.

NOT required for dialog captures. `_capture_dialog_client(hwnd)` grabs a dialog,
which has no page rect, so `page_rect_on_canvas` cannot answer for it and a crop
would be meaningless. Those are identified structurally -- by which capture
helper produced the image -- not by a name pattern, because a name pattern is
what caused the original miss.

Driven off the AST rather than a regex so a reformatted call, a renamed baseline
or a line-wrapped argument list cannot slip past.
"""
from __future__ import annotations

import ast
import io
import textwrap
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET = REPO_ROOT / "tests" / "test_ui_file_operations.py"

CANVAS_CAPTURE = "_capture_canvas"
DIALOG_CAPTURE = "_capture_dialog_client"
BASELINE_CHECK = "_baseline_check"


def _call_name(node: ast.AST) -> str | None:
    if not isinstance(node, ast.Call):
        return None
    fn = node.func
    if isinstance(fn, ast.Name):
        return fn.id
    if isinstance(fn, ast.Attribute):
        return fn.attr
    return None


def _collect(tree: ast.AST):
    """Per-FUNCTION map of variable -> capture helper, plus every check call.

    PER FUNCTION, NOT MODULE-WIDE, and that is load-bearing. Every test in this
    file names its capture `img`, so a module-wide walk puts `img` in BOTH the
    canvas and the dialog set and then reports the dialog oracles as uncropped
    offenders. The first version of this guard did exactly that -- the same
    wrong-population mistake it exists to catch, one level up.

    Returns (canvas_vars, dialog_vars, checks) where the var maps are keyed by
    (function name, variable name) so two functions reusing `img` cannot be
    confused for each other.
    """
    canvas_vars: dict[tuple[str, str], int] = {}
    dialog_vars: dict[tuple[str, str], int] = {}
    checks = []  # (enclosing function name, call node)

    for fn in ast.walk(tree):
        if not isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        for node in ast.walk(fn):
            if isinstance(node, ast.Assign) and len(node.targets) == 1:
                tgt = node.targets[0]
                name = _call_name(node.value)
                if isinstance(tgt, ast.Name) and name == CANVAS_CAPTURE:
                    canvas_vars[(fn.name, tgt.id)] = node.lineno
                elif isinstance(tgt, ast.Name) and name == DIALOG_CAPTURE:
                    dialog_vars[(fn.name, tgt.id)] = node.lineno
            if _call_name(node) == BASELINE_CHECK:
                checks.append((fn.name, node))
    return canvas_vars, dialog_vars, checks


@pytest.fixture(scope="module")
def parsed():
    assert TARGET.is_file(), TARGET
    src = io.open(TARGET, encoding="utf-8").read()
    return _collect(ast.parse(src))


def test_the_probe_can_see_the_call_sites(parsed):
    """Guard the guard: if the AST walk finds nothing, everything below is vacuous."""
    canvas_vars, _dialog_vars, checks = parsed
    assert canvas_vars, (
        "no variable in %s is assigned from %s() -- the capture helper was "
        "renamed, so this guard is inspecting nothing and would pass an "
        "entirely uncropped file" % (TARGET.name, CANVAS_CAPTURE))
    assert checks, (
        "no %s() call found -- the oracle was renamed and this guard is "
        "vacuous" % BASELINE_CHECK)


def _image_arg(call: ast.Call):
    """The node passed as `_baseline_check`'s image, positionally or by keyword."""
    if len(call.args) >= 2:
        return call.args[1]
    for kw in call.keywords:
        if kw.arg == "actual":
            return kw.value
    return None


def _classify(fn_name: str, node, canvas_vars, dialog_vars) -> str:
    """'canvas' | 'dialog' | 'unknown' for the image argument of one check.

    THIS FUNCTION EXISTS BECAUSE THE FIRST VERSION SAID `continue`.
    It skipped any call whose image argument was not a bare Name, so
    `_baseline_check("x", _capture_canvas(s))` written inline -- no intermediate
    variable, an entirely ordinary style -- was invisible to the guard whose
    whole job is to catch that call. check_reachability_skip_shape.py flagged the
    `continue` as a lookup guard whose failure branch does not fail the test
    (WIN-45), and it was right: silently skipping an unparsed shape is how a
    guard reports green about code it never looked at.
    """
    if isinstance(node, ast.Name):
        if (fn_name, node.id) in canvas_vars:
            return "canvas"
        if (fn_name, node.id) in dialog_vars:
            return "dialog"
        return "unknown"
    if isinstance(node, ast.Call):
        name = _call_name(node)
        if name == CANVAS_CAPTURE:
            return "canvas"
        if name == DIALOG_CAPTURE:
            return "dialog"
    return "unknown"


def test_every_canvas_baseline_check_passes_a_crop(parsed):
    canvas_vars, dialog_vars, checks = parsed

    # CLASSIFY EVERY CALL FIRST, then filter -- deliberately with no `continue`.
    # check_reachability_skip_shape.py flags a `continue` inside a lookup guard as
    # the WIN-45 shape (a failure branch that does not fail), and it cannot tell a
    # legitimate exemption from a silent skip. The honest answer is to not have
    # the branch: partition the calls, then assert on each partition. Adding this
    # file to reachability_skip_baseline.json would also have silenced it, but
    # that baseline is being burned DOWN (#229) and growing it to quiet a guard of
    # my own making is the wrong direction.
    triples = [(fn, call, _classify(fn, _image_arg(call), canvas_vars, dialog_vars))
               for fn, call in checks]

    unknown = ["line %d: %s" % (call.lineno,
                                "no image argument found" if _image_arg(call) is None
                                else ast.dump(_image_arg(call))[:120])
               for _fn, call, kind in triples if kind == "unknown"]
    canvas = [call for _fn, call, kind in triples if kind == "canvas"]
    checked = len(canvas)
    offenders = ["line %d: %s" % (call.lineno,
                                 call.args[0].value
                                 if call.args and isinstance(call.args[0], ast.Constant)
                                 else "<unknown>")
                 for call in canvas
                 if not any(kw.arg == "crop" for kw in call.keywords)]

    # An UNRECOGNISED shape is a finding, not something to skip past. If this
    # guard cannot tell what produced the image, it cannot tell whether a crop is
    # required, and passing would be a green about an uninspected call site.
    assert not unknown, (
        "this guard could not classify the image argument of these "
        "_baseline_check calls, so it cannot say whether they need a crop:\n  "
        + "\n  ".join(unknown) +
        "\nEither assign the capture to a local first, or extend _classify() to "
        "recognise the shape. Do NOT let it fall through silently -- that is the "
        "WIN-45 pattern check_reachability_skip_shape.py exists to catch.")

    assert checked > 0, (
        "no _baseline_check call was matched to a _capture_canvas image, so "
        "this guard asserted nothing")
    assert not offenders, (
        "these canvas oracles compare a FULL canvas capture against a baseline "
        "without cropping to the page rect, so they assert the session theme "
        "(QPalette::Dark vs QPalette::Window via AccessibilityHelper::adaptColor) "
        "and not the rendering:\n  " + "\n  ".join(offenders) +
        "\nAdd crop=_page_area_crop(s), or capture a dialog instead.")


def test_the_three_classifications_are_each_reachable():
    """Synthetic source, because the live file happens to use only ONE shape.

    Every call in test_ui_file_operations.py assigns its capture to a local, so
    the live file exercises exactly one branch of `_classify`. That makes the
    inline-call and unrecognised-shape branches untested by the real sweep, and
    an untested branch in a guard is the guard's own blind spot -- which is how
    the first version shipped with a silent `continue`.
    """
    src = textwrap.dedent("""
        def test_inline(s):
            _baseline_check("inline_no_crop", _capture_canvas(s))

        def test_odd_shape(s):
            _baseline_check("odd", mystery_helper(s), crop=1)

        def test_dialog(h):
            img = _capture_dialog_client(h)
            _baseline_check("dlg", img)
    """)
    canvas_vars, dialog_vars, checks = _collect(ast.parse(src))
    kinds = {fn: _classify(fn, _image_arg(call), canvas_vars, dialog_vars)
             for fn, call in checks}

    assert kinds["test_inline"] == "canvas", (
        "an INLINE _capture_canvas(...) passed straight to _baseline_check was "
        "not recognised as a canvas capture. That shape needs no intermediate "
        "variable and is what the original `continue` skipped silently.")
    assert kinds["test_odd_shape"] == "unknown", (
        "an unrecognised image expression was classified as something known, so "
        "the guard would judge a call site it cannot actually see")
    assert kinds["test_dialog"] == "dialog", kinds


def test_dialog_captures_are_not_required_to_crop(parsed):
    """The exemption is structural, and must stay structural.

    If dialog captures ever start being matched as canvas ones, the guard above
    would demand an impossible crop and someone would weaken it. Pin that the two
    helpers are distinguished.
    """
    canvas_vars, dialog_vars, _checks = parsed
    assert dialog_vars, (
        "no %s() assignment found -- either the dialog helper was renamed, or "
        "the dialog oracles now go through the canvas path, in which case the "
        "exemption in this guard needs revisiting" % DIALOG_CAPTURE)
    # Keys are (function, variable). A clash would mean ONE function assigns the
    # same name from both helpers, which really would be ambiguous. A bare
    # variable-name comparison is NOT the right test: every test here calls its
    # capture `img`, so that would always clash and the guard would be
    # permanently red for a reason that has nothing to do with cropping.
    assert not (set(canvas_vars) & set(dialog_vars)), (
        "one function assigns the same variable from BOTH capture helpers; the "
        "structural distinction this guard relies on no longer holds")
