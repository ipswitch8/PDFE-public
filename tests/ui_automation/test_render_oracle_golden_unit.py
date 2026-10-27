# -*- coding: utf-8 -*-
"""Headless unit tests for the golden-image oracle (gap A1-68 / UI-ORACLE-02).

These require NO live PDFE, no display and no UIA — they drive
``render_oracle.matches_golden`` with synthetic Pillow images on tmp paths, so
they run in any CI lane.

The defect they lock out
========================

``matches_golden`` used to contain::

    if not p.exists():
        save_golden(img, p)
        return True

A missing golden silently became the defended baseline AND the call reported
PASS having compared nothing. Every assertion below names the mutation it
kills; each one fails if that code is restored or the policy is weakened.
"""
from __future__ import annotations

import io
import pathlib
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from PIL import Image  # noqa: E402

from tests.ui_automation.harness import render_oracle as ro  # noqa: E402


def _img(colour=(30, 60, 200), size=(64, 48)):
    return Image.new("RGB", size, colour)


def _textured(seed: int = 0, size=(64, 48)):
    """A non-uniform image so SSIM has real structure to compare."""
    im = Image.new("RGB", size, (255, 255, 255))
    px = im.load()
    for y in range(size[1]):
        for x in range(size[0]):
            if (x + y + seed) % 7 < 2:
                px[x, y] = (0, 0, 0)
    return im


@pytest.fixture
def captured_stderr(monkeypatch):
    """Capture stderr without pytest's capsys — pytest.ini sets
    ``-p no:capture`` repo-wide, which disables the capsys fixture."""
    buf = io.StringIO()
    monkeypatch.setattr(sys, "stderr", buf)
    return buf


@pytest.fixture(autouse=True)
def _clear_seed_state(monkeypatch):
    """Each test starts with no seeding env var and an empty seed ledger."""
    monkeypatch.delenv(ro.SEED_ENV_VAR, raising=False)
    ro.SEEDED_GOLDENS.clear()
    yield
    ro.SEEDED_GOLDENS.clear()


# ---------------------------------------------------------------------------
# 1. Missing golden must NOT pass silently
# ---------------------------------------------------------------------------

def test_missing_golden_raises_and_does_not_write(tmp_path):
    """MUTATION KILLED: restoring ``if not p.exists(): save_golden(); return
    True``. That mutation makes this call return True and create the file;
    both assertions below then fail."""
    p = tmp_path / "never_seeded.png"
    with pytest.raises(ro.GoldenMissingError):
        ro.matches_golden(_textured(), p)
    assert not p.exists(), (
        "a default-path comparison must not create the golden it was supposed "
        "to compare against")
    assert ro.SEEDED_GOLDENS == []


def test_missing_golden_error_names_the_seeding_route(tmp_path):
    """MUTATION KILLED: replacing the explanatory error with a bare
    ``raise RuntimeError()`` / ``return False``. The message must tell the
    operator how to seed deliberately, otherwise the fix just looks like a
    flaky failure and gets 'fixed' by re-adding auto-seed."""
    with pytest.raises(ro.GoldenMissingError) as exc:
        ro.matches_golden(_textured(), tmp_path / "missing.png")
    msg = str(exc.value)
    assert ro.SEED_ENV_VAR in msg
    assert "seed_golden" in msg


def test_missing_golden_raises_even_when_env_seeding_forced_off(
        tmp_path, monkeypatch):
    """MUTATION KILLED: making ``seed_if_missing=False`` a no-op / letting the
    env var win over an explicit per-call refusal."""
    monkeypatch.setenv(ro.SEED_ENV_VAR, "1")
    p = tmp_path / "explicit_off.png"
    with pytest.raises(ro.GoldenMissingError):
        ro.matches_golden(_textured(), p, seed_if_missing=False)
    assert not p.exists()


# ---------------------------------------------------------------------------
# 2. Matching golden passes
# ---------------------------------------------------------------------------

def test_matching_golden_passes(tmp_path):
    """MUTATION KILLED: inverting the comparison, or making matches_golden
    unconditionally raise/return False (a 'fix' that just breaks the lane)."""
    p = tmp_path / "g.png"
    img = _textured()
    ro.save_golden(img, p)
    assert ro.matches_golden(img, p) is True


def test_matching_golden_does_not_record_a_seed(tmp_path):
    """MUTATION KILLED: seeding on every call (writing the capture over the
    golden before comparing), which would make ANY capture match."""
    p = tmp_path / "g.png"
    ro.save_golden(_textured(), p)
    ro.matches_golden(_textured(), p)
    assert ro.SEEDED_GOLDENS == [], "a verification run must seed nothing"


# ---------------------------------------------------------------------------
# 3. Differing golden fails
# ---------------------------------------------------------------------------

def test_differing_golden_fails(tmp_path):
    """MUTATION KILLED: ``return True`` anywhere on the compare path, or
    raising GOLDEN_SSIM_MIN/tolerance to a value that accepts anything."""
    p = tmp_path / "g.png"
    ro.save_golden(Image.new("RGB", (64, 48), (0, 0, 0)), p)
    assert ro.matches_golden(Image.new("RGB", (64, 48), (255, 255, 255)),
                             p) is False


def test_differing_golden_fails_even_in_seeding_mode(tmp_path, monkeypatch):
    """MUTATION KILLED: making PDFE_SEED_GOLDENS overwrite an EXISTING golden.
    Seeding mode must only cover the *missing* case; if it re-seeded existing
    goldens, a real regression would be laundered into a new baseline."""
    monkeypatch.setenv(ro.SEED_ENV_VAR, "1")
    p = tmp_path / "g.png"
    black = Image.new("RGB", (64, 48), (0, 0, 0))
    ro.save_golden(black, p)
    white = Image.new("RGB", (64, 48), (255, 255, 255))
    assert ro.matches_golden(white, p) is False
    # ...and the committed golden is untouched.
    assert Image.open(str(p)).convert("RGB").getpixel((0, 0)) == (0, 0, 0)


# ---------------------------------------------------------------------------
# 4. Explicit seeding mode seeds AND reports that it seeded
# ---------------------------------------------------------------------------

def test_seed_if_missing_true_seeds_and_reports(tmp_path, captured_stderr):
    """MUTATION KILLED: seeding silently (dropping the SEEDED_GOLDENS record
    or the stderr notice) — that is exactly what made the old behaviour
    invisible."""
    p = tmp_path / "sub" / "seed.png"
    img = _textured(seed=3)
    assert ro.matches_golden(img, p, seed_if_missing=True) is True
    assert p.exists(), "seeding mode must actually write the golden"
    assert p in ro.SEEDED_GOLDENS, (
        "a seeded golden must be recorded so a seeding run can never be "
        "reported as a verification run")
    err = captured_stderr.getvalue()
    assert "SEEDED GOLDEN" in err and "NOT VERIFIED" in err


def test_env_var_enables_seeding(tmp_path, monkeypatch):
    """MUTATION KILLED: ignoring PDFE_SEED_GOLDENS (no bulk-seeding route), or
    treating any value as truthy."""
    p = tmp_path / "env_seed.png"
    monkeypatch.setenv(ro.SEED_ENV_VAR, "0")
    assert ro.seeding_enabled() is False
    with pytest.raises(ro.GoldenMissingError):
        ro.matches_golden(_textured(), p)
    monkeypatch.setenv(ro.SEED_ENV_VAR, "1")
    assert ro.seeding_enabled() is True
    assert ro.matches_golden(_textured(), p) is True
    assert p.exists() and p in ro.SEEDED_GOLDENS


def test_seed_golden_entry_point_returns_path_and_records(tmp_path,
                                                          captured_stderr):
    """MUTATION KILLED: removing the dedicated seed entry point (forcing
    callers back to the implicit path)."""
    p = tmp_path / "direct.png"
    out = ro.seed_golden(_textured(), p)
    assert out == p and p.exists()
    assert ro.SEEDED_GOLDENS == [p]
    assert "SEEDED GOLDEN" in captured_stderr.getvalue()


def test_seeded_golden_then_verifies_against_itself(tmp_path):
    """Documents the contract: after a deliberate seed, the NEXT call is a
    real comparison (not another seed).
    MUTATION KILLED: leaving the seeding flag latched on for the process."""
    p = tmp_path / "g.png"
    img = _textured()
    ro.matches_golden(img, p, seed_if_missing=True)
    ro.SEEDED_GOLDENS.clear()
    assert ro.matches_golden(img, p) is True
    assert ro.SEEDED_GOLDENS == []
    assert ro.matches_golden(Image.new("RGB", img.size, (255, 0, 0)),
                             p) is False


# ---------------------------------------------------------------------------
# 5. The caller wrapper inherits the policy
# ---------------------------------------------------------------------------

def test_render_drive_assert_matches_golden_errors_on_missing(monkeypatch,
                                                              tmp_path):
    """MUTATION KILLED: re-adding auto-seeding at the render_drive wrapper
    level (i.e. fixing the oracle but leaving the caller self-seeding)."""
    rd = pytest.importorskip(
        "tests.ui_automation.harness.render_drive",
        reason="render_drive requires the pywinauto harness deps")
    monkeypatch.setattr(rd, "GOLDENS", tmp_path)
    with pytest.raises(ro.GoldenMissingError):
        rd.assert_matches_golden(_textured(), "no_such_golden.png")
    assert not (tmp_path / "no_such_golden.png").exists()
