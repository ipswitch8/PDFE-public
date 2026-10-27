# -*- coding: utf-8 -*-
"""PLANTED CONTROL (F35, criterion 5) — legitimate gates. MUST NOT be flagged.

The discriminating half of the pair with `win45_positive_reachability_skip.py`.
A checker that flagged everything would satisfy the positive control and be
useless; these are the guards it must leave alone.

Every guard below is **provenance-gated**: its condition derives from a call the
checker can recognise as environmental (`shutil.which`, `importlib.util.find_spec`,
`os.environ.get`, a filesystem-existence probe on a value that is provably a
path). The externality is expressed in the code, not only in the prose, which is
precisely what makes it checkable.

The form the checker canNOT see — `acrobat = _find_acrobat()`, where the
externality is hidden behind a call into a helper — is deliberately NOT in this
file. It is the checker's documented limitation, it stays in
`reachability_skip_baseline.json`, and
`tools/gate/test_f35_tranche_controls.py::test_opaque_external_tool_helper_is_still_flagged`
pins that fact so the limitation cannot quietly become a hole.
"""

import importlib.util
import os
import shutil
from pathlib import Path

import pytest

FIXTURES = Path(__file__).resolve().parent / "data"


def test_external_tool_absent_is_a_real_environment_gate():  # pragma: no cover
    acrobat = shutil.which("Acrobat.exe")
    if acrobat is None:
        pytest.skip("Acrobat is not installed on this machine.")
    assert Path(acrobat).name


def test_optional_dependency_absent_is_a_real_environment_gate():  # pragma: no cover
    if importlib.util.find_spec("pikepdf") is None:
        pytest.skip("pikepdf not installed.")
    assert True


def test_opt_in_env_var_is_a_real_environment_gate():  # pragma: no cover
    if os.environ.get("PDFE_RUN_LIVE_UI") != "1":
        pytest.skip("live-UI suite is opt-in.")
    assert True


def test_missing_fixture_file_is_a_real_environment_gate():  # pragma: no cover
    fixture = FIXTURES / "sample.pdf"
    if not fixture.exists():
        pytest.skip("corpus fixture not present in this checkout.")
    assert fixture.stat().st_size > 0
