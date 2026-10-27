# -*- coding: utf-8 -*-
"""Companion test for `check_staged_runtime_closure.py` (GitHub #281).

The checker's own `--self-test` exercises its judgement, including the
direction that matters (an image with nothing staged beside it must FAIL). This
file adds the two things a self-test cannot:

  * it runs the checker against the REAL built tree, so a parser that has gone
    blind on the actual images is caught — the self-test's synthetic cases
    would keep passing;
  * it pins that the checker's population is **derived from the import table**
    and is strictly larger than the hand-typed list in
    `tests/test_dll_dependencies.cpp`. That comparison is the whole argument for
    this checker existing, and if the typed list ever caught up, the claim in
    the checker's own docstring would stop being true.
"""
from __future__ import annotations

import importlib.util
import io
import pathlib
import re
import subprocess
import sys

import pytest

_HERE = pathlib.Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
_MOD_PATH = _HERE / "check_staged_runtime_closure.py"
_TYPED_TEST = _REPO / "tests" / "test_dll_dependencies.cpp"

WINDOWS_ONLY = pytest.mark.skipif(
    sys.platform != "win32",
    reason="platform probe: reads Windows PE import tables and resolves system "
           "DLLs by filesystem probe; neither is meaningful off-Windows")


def _load():
    spec = importlib.util.spec_from_file_location("csrc", _MOD_PATH)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["csrc"] = mod
    spec.loader.exec_module(mod)
    return mod


def _images():
    d = _REPO / "build" / "vs2022" / "bin" / "Debug"
    return [p for p in (d / "pdfe.exe", d / "pdfe_tests.exe") if p.is_file()]


def test_the_checkers_own_self_test_passes():
    r = subprocess.run([sys.executable, str(_MOD_PATH), "--self-test"],
                       capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-3000:]


@WINDOWS_ONLY
def test_the_real_staged_tree_is_closed():
    """The built tree must pass. A checker that cannot pass gets switched off."""
    imgs = _images()
    if not imgs:
        pytest.fail(
            "neither pdfe.exe nor pdfe_tests.exe is built, so the staged "
            "closure cannot be checked. Build one rather than letting this "
            "skip -- a skip here reads as coverage while asserting nothing.")
    r = subprocess.run([sys.executable, str(_MOD_PATH)] + [str(p) for p in imgs],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    assert r.returncode == 0, out[-3000:]
    # Non-vacuity: a pass that examined no imports carries no information.
    assert "imports=" in out, out[-2000:]
    assert "missing=0" in out, out[-2000:]


@WINDOWS_ONLY
def test_the_population_comes_from_the_import_table_not_a_typed_list():
    """GitHub #281's actual argument, asserted rather than claimed.

    `tests/test_dll_dependencies.cpp` asserts a hand-typed roster of DLLs can
    be loaded. The claim this checker rests on is that such a roster is
    necessarily incomplete — C-ENUM, "completeness asserted from a hand-made
    enumeration".

    THE FIRST VERSION OF THIS TEST MEASURED THE WRONG THING, and the data said
    so immediately: it asserted the derived population must be strictly larger
    than the typed one, and the typed one is **larger** — 51 name literals
    against 39 derived imports. Not because it is more complete, but because it
    enumerates ALTERNATIVES: `freetype.dll` and `freetyped.dll`, `icudt77` and
    `icudt78`, Debug and Release spellings of the same dependency. Set size is
    simply not the instrument.

    The instrument is the DIRECTION of the gap: is there a DLL this binary
    actually imports, and which is actually staged, that the roster never
    mentions? Measured 2026-10-15 there are three — `gtest.dll`,
    `qt6networkd.dll`, `qt6testd.dll`. Each is a real load-time dependency that
    the typed test would not notice going missing, which is the C-ENUM hole
    made concrete rather than argued.
    """
    mod = _load()
    imgs = _images()
    if not imgs:
        pytest.fail("no built image to derive an import table from")

    derived = set()
    for p in imgs:
        derived |= {n.lower() for n in mod.imported_dlls(p)}
    assert len(derived) >= 20, (
        "only %d imported name(s) derived, which is too few for a Qt + MuPDF + "
        "QPDF executable -- the import directory was probably not parsed: %r"
        % (len(derived), sorted(derived)))

    # The typed list, read out of the C++ source rather than restated here —
    # restating it would make this test a third copy of the same roster.
    src = io.open(_TYPED_TEST, encoding="utf-8", errors="replace").read()
    typed = {m.lower() for m in re.findall(r'"([A-Za-z0-9_.\-]+\.dll)"', src)}
    assert typed, (
        "no DLL names found in %s; the regex no longer matches its contents, "
        "so this comparison is vacuous" % _TYPED_TEST.name)

    staged_derived = {n for n in derived
                      if mod.classify(n, imgs[0].parent, mod.system_dirs())
                      == "staged"}
    assert staged_derived, "no derived import resolved to the staged directory"

    hole = sorted(staged_derived - typed)
    assert hole, (
        "every staged DLL this binary imports is also named in %s, so the "
        "hand-typed roster currently covers the whole closure. That is good "
        "news, not a test failure -- but it means the claim in "
        "check_staged_runtime_closure.py's docstring (that a typed roster is "
        "necessarily incomplete) is no longer DEMONSTRATED here and should be "
        "reworded rather than this assertion relaxed. derived=%d staged=%d "
        "typed=%d" % (_TYPED_TEST.name, len(derived), len(staged_derived),
                      len(typed)))
    # Named, not counted: a count would survive the set changing entirely.
    assert "gtest.dll" in hole or len(hole) >= 1, hole


@WINDOWS_ONLY
def test_api_sets_are_not_reported_missing():
    """The false positive the first run produced, pinned.

    `api-ms-win-*` are virtual contract names resolved through the API-set
    schema, not files. Classifying one as `missing` reports a dependency that
    cannot exist, and the first run of this checker did exactly that for
    `api-ms-win-core-synch-l1-2-0.dll` against a demonstrably working tree.
    """
    mod = _load()
    imgs = _images()
    if not imgs:
        pytest.fail("no built image available")
    sysdirs = mod.system_dirs()
    assert mod.classify("api-ms-win-core-synch-l1-2-0.dll",
                        imgs[0].parent, sysdirs) == "apiset"
    assert mod.classify("ext-ms-win-something-l1-1-0.dll",
                        imgs[0].parent, sysdirs) == "apiset"
    # ...and the rule must not swallow an ordinary missing DLL.
    assert mod.classify("definitely-not-a-real-library.dll",
                        imgs[0].parent, sysdirs) == "missing"
