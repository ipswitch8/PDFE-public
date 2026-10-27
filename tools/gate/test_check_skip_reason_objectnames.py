#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-test for ``tools/gate/check_skip_reason_objectnames.py``.

Pins BOTH directions. A guard that cannot redden is decoration, and a guard
that can only redden is equally useless — this repo has shipped both kinds.

  * ``test_planted_false_skip_reason_is_caught`` — a skip reason naming an
    objectName that ``src/`` really defines must be reported.
  * ``test_legitimate_skip_reason_is_not_caught`` — a skip reason naming an
    objectName that genuinely does not exist anywhere in ``src/`` must be
    left alone, as must a runtime-reachability reason and a docstring/comment
    that merely QUOTES a historical false skip.
  * ``test_self_match_exclusion_is_load_bearing`` — the decoys at the bottom
    of this file are caught when the exclusion is bypassed and silent when it
    is not, so "the guard is green on tools/gate" is a real statement.

Run: ``python -m pytest tools/gate/test_check_skip_reason_objectnames.py -v``
(or ``python tools/gate/test_check_skip_reason_objectnames.py`` for a
pytest-free run).
"""
from __future__ import annotations

import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_skip_reason_objectnames as guard  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[2]

#: A name src/ really defines — src/ui/SignDialog.cpp:178. This is the exact
#: identifier GitHub #209's worst skip reason claimed was "not in this build".
REAL_OBJECT_NAME = "invisibleCheckBox"

#: A name no PDFE source has ever defined. Asserted below, not assumed.
FICTIONAL_OBJECT_NAME = "quimbyWidgetThatHasNeverExisted"


def _object_names():
    names = guard.collect_src_object_names(REPO_ROOT / "src")
    assert names, "no setObjectName literals found under src/"
    return names


def _write(tmpdir: Path, body: str) -> Path:
    p = tmpdir / "test_planted_case.py"
    p.write_text(body, encoding="utf-8")
    return p


def test_fixture_assumptions_hold():
    """The two names this file is built on must really be what it says."""
    names = _object_names()
    assert REAL_OBJECT_NAME in names, (
        f"{REAL_OBJECT_NAME} is no longer defined in src/ — the RED direction "
        f"below would pass vacuously. Re-pick a real objectName."
    )
    assert FICTIONAL_OBJECT_NAME not in names, (
        f"{FICTIONAL_OBJECT_NAME} is now defined in src/ — the GREEN "
        f"direction below would fail for the wrong reason."
    )


def test_planted_false_skip_reason_is_caught(tmp_path):
    """RED direction: a false absence claim must be reported."""
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "def test_planted():\n"
        f"    pytest.skip('{REAL_OBJECT_NAME} not in this build')\n"
    )
    findings = guard.scan(_write(tmp_path, src), _object_names())
    assert len(findings) == 1, f"expected 1 finding, got {findings!r}"
    assert findings[0].ident == REAL_OBJECT_NAME


def test_planted_absent_from_phrasing_is_caught(tmp_path):
    """RED direction, second phrasing — the backticked ``absent from`` shape
    that GitHub #209's seventh group used."""
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "def test_planted():\n"
        f"    pytest.skip('palette action `{REAL_OBJECT_NAME}` absent from "
        "the product')\n"
    )
    findings = guard.scan(_write(tmp_path, src), _object_names())
    assert [f.ident for f in findings] == [REAL_OBJECT_NAME], findings


def test_planted_reason_kwarg_is_caught(tmp_path):
    """RED direction, third surface — ``@pytest.mark.skipif(reason=...)``."""
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "@pytest.mark.skipif(True, "
        f"reason='{REAL_OBJECT_NAME} is not implemented')\n"
        "def test_planted():\n"
        "    pass\n"
    )
    findings = guard.scan(_write(tmp_path, src), _object_names())
    assert [f.ident for f in findings] == [REAL_OBJECT_NAME], findings


def test_stale_rename_is_caught(tmp_path):
    """RED direction, the shape that actually happened in GitHub #209.

    ``invisibleCheck`` is NOT an objectName anywhere in src/ — the commit the
    skip reason was describing renamed it to ``invisibleCheckBox``. An
    exact-membership guard calls that reason truthful and misses the bug.
    """
    names = _object_names()
    assert "invisibleCheck" not in names, (
        "invisibleCheck is now a real objectName — this test would then be "
        "exercising the exact-match path, not the rename path"
    )
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "def test_stale():\n"
        "    pytest.skip('invisibleCheck not in this build')\n"
    )
    findings = guard.scan(_write(tmp_path, src), names)
    assert len(findings) == 1, findings
    assert findings[0].ident == "invisibleCheck"
    assert "invisibleCheckBox" in findings[0].defined_at


def test_fstring_reason_bound_to_a_local_is_caught(tmp_path):
    """RED direction: GitHub #209's seventh group, verbatim in shape — an
    f-string over a parametrize value, bound to a local before being passed."""
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "@pytest.mark.parametrize('action_id', "
        "['formTextFieldAction', 'formCheckboxAction'])\n"
        "def test_palette(action_id):\n"
        "    _reach = f'palette action {action_id} absent from UIA tree'\n"
        "    pytest.skip(_reach)\n"
    )
    findings = guard.scan(_write(tmp_path, src), _object_names())
    assert {f.ident for f in findings} == {
        "formTextFieldAction", "formCheckboxAction"}, findings


def test_local_reason_does_not_leak_across_functions(tmp_path):
    """Reason variables are resolved PER FUNCTION. ``_reach`` is rebound in
    nearly every test in these files; a module-wide map would attribute one
    function's reason to another function's skip and invent findings."""
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "def test_a():\n"
        "    _reach = 'invisibleCheckBox not in this build'\n"
        "    assert _reach\n"
        "def test_b():\n"
        f"    _reach = '{FICTIONAL_OBJECT_NAME} not in this build'\n"
        "    pytest.skip(_reach)\n"
    )
    assert guard.scan(_write(tmp_path, src), _object_names()) == []


def test_fail_surface_is_out_of_scope(tmp_path):
    """GREEN direction: ``pytest.fail`` is not a skip. A red test costs
    nothing if its message is imprecise; only a GREEN one hides coverage."""
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "def test_red():\n"
        f"    pytest.fail('{REAL_OBJECT_NAME} not in this build')\n"
    )
    assert guard.scan(_write(tmp_path, src), _object_names()) == []


def test_legitimate_skip_reason_is_not_caught(tmp_path):
    """GREEN direction: a skip naming something src/ genuinely lacks stands."""
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "def test_legit():\n"
        f"    pytest.skip('{FICTIONAL_OBJECT_NAME} not in this build')\n"
    )
    assert guard.scan(_write(tmp_path, src), _object_names()) == []


def test_runtime_reachability_phrasing_is_not_caught(tmp_path):
    """GREEN direction: "not in UIA tree" is a runtime observation about a
    widget that may well be defined, and is deliberately out of scope — see
    the comment beside _TRAILING_PHRASES."""
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "def test_reach():\n"
        f"    pytest.skip('{REAL_OBJECT_NAME} not in UIA tree')\n"
    )
    assert guard.scan(_write(tmp_path, src), _object_names()) == []


def test_unrelated_names_are_not_treated_as_renames(tmp_path):
    """GREEN direction: the rename heuristic must not fire on two widgets that
    merely share a suffix. ``cancelButton`` is defined; a skip about a
    fictional ``quimbyButton`` must stand."""
    names = _object_names()
    assert "cancelButton" in names
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        "def test_legit():\n"
        "    pytest.skip('quimbyButton not in this build')\n"
    )
    assert guard.scan(_write(tmp_path, src), names) == []


def test_comment_and_docstring_quoting_history_is_not_caught(tmp_path):
    """GREEN direction: the #209 fixes document the false reasons they
    replaced, in comments and docstrings. Those must not re-trigger the guard
    or every fix would redden the check that motivated it."""
    src = (
        "# -*- coding: utf-8 -*-\n"
        "import pytest\n"
        f"# history: this used to say pytest.skip('{REAL_OBJECT_NAME} not in "
        "this build')\n"
        "def test_fixed():\n"
        '    """The old reason was: '
        f"{REAL_OBJECT_NAME} not in this build. It was false.\"\"\"\n"
        "    assert True\n"
    )
    assert guard.scan(_write(tmp_path, src), _object_names()) == []


def test_self_match_exclusion_is_load_bearing():
    """The decoys below are caught with the exclusion bypassed and silent
    with it on. Without the first half, "green on tools/gate" would be a
    zero-against-zero comparison."""
    gate_dir = Path(__file__).resolve().parent
    names = _object_names()
    bypassed = guard.scan(gate_dir, names, bypass_exclusion=True)
    assert bypassed, (
        "no decoy found in tools/gate with the exclusion bypassed — the "
        "exclusion proof would be vacuous"
    )
    assert REAL_OBJECT_NAME in {f.ident for f in bypassed}
    assert guard.scan(gate_dir, names) == [], (
        "the guard flagged its own source/self-test — self-match exclusion "
        "is broken"
    )


def test_guard_errors_when_src_yields_nothing(tmp_path):
    """A guard that passes when it scanned nothing is the classic vacuous
    green. Pointing --src at an empty tree must exit 2, not 0."""
    (tmp_path / "src").mkdir()
    (tmp_path / "tests").mkdir()
    rc = guard.main(["--repo", str(tmp_path), "--src", "src",
                     "--root", "tests"])
    assert rc == 2, f"expected exit 2 on an empty src/, got {rc}"


# ---------------------------------------------------------------------------
# DECOYS — live skip-reason calls carrying false absence claims, used by
# test_self_match_exclusion_is_load_bearing above. They are inside a function
# that is never called and never collected as a test (no ``test_`` prefix), so
# they cannot skip anything; they exist purely so the exclusion has something
# real to exclude.
# ---------------------------------------------------------------------------
def _decoy_never_called():
    import pytest
    pytest.skip("invisibleCheckBox not in this build")
    pytest.skip("certificationLevelCombo is not implemented")
    pytest.skip("`themeLightAction` absent from the product")


if __name__ == "__main__":
    try:
        import pytest as _pytest
    except ImportError:
        _pytest = None
    if _pytest is not None:
        sys.exit(_pytest.main([__file__, "-v"]))
    # pytest-free fallback so the guard can be self-checked anywhere.
    import tempfile
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            if fn.__code__.co_argcount:
                with tempfile.TemporaryDirectory() as td:
                    fn(Path(td))
            else:
                fn()
            print(f"PASS {name}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"FAIL {name}: {exc}")
    sys.exit(1 if failures else 0)
